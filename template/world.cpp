#include "precomp.h"
#include "bluenoise.h"

using namespace Tmpl8;

static const uint gridSize = GRIDSIZE * sizeof( uint );
static const uint commitSize = BRICKCOMMITSIZE + gridSize;


// helper defines for inline ray tracing
#define OFFS_X		((bits >> 5) & 1)			// extract grid plane offset over x (0 or 1)
#define OFFS_Y		((bits >> 13) & 1)			// extract grid plane offset over y (0 or 1)
#define OFFS_Z		(bits >> 21)				// extract grid plane offset over z (0 or 1)
#define DIR_X		((bits & 3) - 1)			// ray dir over x (-1 or 1)
#define DIR_Y		(((bits >> 8) & 3) - 1)		// ray dir over y (-1 or 1)
#define DIR_Z		(((bits >> 16) & 3) - 1)	// ray dir over z (-1 or 1)
#define BMSK		(BRICKDIM - 1)
#define BDIM2		(BRICKDIM * BRICKDIM)
#define BPMX		(MAPWIDTH - BRICKDIM)
#define BPMY		(MAPHEIGHT - BRICKDIM)
#define BPMZ		(MAPDEPTH - BRICKDIM)
#define TOPMASK3	(((1023 - BMSK) << 20) + ((1023 - BMSK) << 10) + (1023 - BMSK))
#define SELECT(a,b,c) ((c)?(b):(a))

// World Constructor
// ----------------------------------------------------------------------------
World::World( const uint targetID )
{
	// create the commit buffer, used to sync CPU-side changes to the GPU
	if (!Kernel::InitCL()) FATALERROR( "Failed to initialize OpenCL" );
	devmem = clCreateBuffer( Kernel::GetContext(), CL_MEM_READ_ONLY, commitSize, 0, 0 );
	modified = new uint[BRICKCOUNT / 32]; // 1 bit per brick, to track 'dirty' bricks
	// store top-level grid in a 3D texture
	cl_image_format fmt;
	fmt.image_channel_order = CL_R;
	fmt.image_channel_data_type = CL_UNSIGNED_INT32;
	cl_image_desc desc;
	memset( &desc, 0, sizeof( cl_image_desc ) );
	desc.image_type = CL_MEM_OBJECT_IMAGE3D;
	desc.image_width = MAPWIDTH / BRICKDIM;
	desc.image_height = MAPHEIGHT / BRICKDIM;
	desc.image_depth = MAPDEPTH / BRICKDIM;
	gridMap = clCreateImage( Kernel::GetContext(), CL_MEM_HOST_NO_ACCESS, &fmt, &desc, 0, 0 );
	// create brick storage
	brick = (uchar*)_aligned_malloc( BRICKCOUNT * BRICKSIZE, 64 );
	backupBrick = (uchar*)_aligned_malloc(BRICKCOUNT * BRICKSIZE, 64);
	brickBuffer = new Buffer( (BRICKCOUNT * BRICKSIZE) / 4, Buffer::DEFAULT, brick );
	brickInfo = new BrickInfo[BRICKCOUNT];
	// create a cyclic array for unused bricks (all of them, for now)
	trash = new uint[BRICKCOUNT];
	memset( trash, 0, BRICKCOUNT * 4 );
	for (uint i = 0; i < BRICKCOUNT; i++) trash[(i * 31 /* prevent false sharing*/) & (BRICKCOUNT - 1)] = i;
	// prepare a test world
	grid = gridOrig = (uint*)_aligned_malloc( GRIDWIDTH * GRIDHEIGHT * GRIDDEPTH * 4, 64 );
	backupGrid = (uint*)_aligned_malloc(GRIDWIDTH * GRIDHEIGHT * GRIDDEPTH * 4, 64);
	dirtyGrid = new bool[GRIDWIDTH * GRIDHEIGHT * GRIDDEPTH * 4];
	memset(dirtyGrid, 0, GRIDWIDTH * GRIDHEIGHT * GRIDDEPTH * 4);
	memset( grid, 0, GRIDWIDTH * GRIDHEIGHT * GRIDDEPTH * sizeof( uint ) );
	memset(backupGrid, 0, GRIDWIDTH * GRIDHEIGHT * GRIDDEPTH * sizeof(uint));
	DummyWorld();
	LoadSky( "assets/sky_15.hdr", "assets/sky_15.bin" );
	brickBuffer->CopyToDevice();
	ClearMarks(); // clear 'modified' bit array
	// report memory usage
	printf( "Allocated %iMB on CPU and GPU for the top-level grid.\n", (int)(gridSize >> 20) );
	printf( "Allocated %iMB on CPU and GPU for %ik bricks.\n", (int)((BRICKCOUNT * BRICKSIZE) >> 20), (int)(BRICKCOUNT >> 10) );
	printf( "Allocated %iKB on CPU for bitfield.\n", (int)(BRICKCOUNT >> 15) );
	printf( "Allocated %iMB on CPU for brickInfo.\n", (int)((BRICKCOUNT * sizeof( BrickInfo )) >> 20) );
	// initialize kernels
	paramBuffer = new Buffer( sizeof( RenderParams ) / 4, Buffer::DEFAULT | Buffer::READONLY, &params );
#if CPUONLY == 0
	screen = new Buffer( targetID, Buffer::TARGET );
	renderer = new Kernel( "cl/kernels.cl", "render" );
	committer = new Kernel( renderer->GetProgram(), "commit" );
	renderer->SetArgument( 0, screen );
	renderer->SetArgument( 1, paramBuffer );
	renderer->SetArgument( 2, &gridMap );
	renderer->SetArgument( 3, brickBuffer );
	renderer->SetArgument( 4, sky );
	committer->SetArgument( 1, &devmem );
	committer->SetArgument( 2, brickBuffer );
	// prepare the bluenoise data
	const uchar* data8 = (const uchar*)sob256_64; // tables are 8 bit per entry
	uint* data32 = new uint[65536 * 5]; // we want a full uint per entry
	for (int i = 0; i < 65536; i++) data32[i] = data8[i]; // convert
	data8 = (uchar*)scr256_64;
	for (int i = 0; i < (128 * 128 * 8); i++) data32[i + 65536] = data8[i];
	data8 = (uchar*)rnk256_64;
	for (int i = 0; i < (128 * 128 * 8); i++) data32[i + 3 * 65536] = data8[i];
	blueNoise = new Buffer( 65536 * 5, Buffer::READONLY, data32 );
	blueNoise->CopyToDevice();
	delete data32;
	renderer->SetArgument( 5, blueNoise );
#endif
	targetTextureID = targetID;
	// load a bitmap font for the print command
	font = new Surface( "assets/font.png" );
}

// World Destructor
// ----------------------------------------------------------------------------
World::~World()
{
	cl_program sharedProgram = renderer->GetProgram();
	delete committer;
	delete renderer;
	_aligned_free( gridOrig ); // grid itself gets changed after allocation
	_aligned_free( brick );
	_aligned_free(backupBrick);
	delete brickBuffer;
	delete brickInfo;
	delete trash;
	delete screen;
	delete paramBuffer;
	delete sky;
	delete blueNoise;
	delete font;
	clReleaseProgram( sharedProgram );
}

// World::ForceSyncAllBricks: send brick array to GPU
// ----------------------------------------------------------------------------
void World::ForceSyncAllBricks()
{
	brickBuffer->CopyToDevice();
}

// World::DummyWorld: box
// ----------------------------------------------------------------------------
void World::DummyWorld()
{
	Clear();
	for (int y = 0; y < 256; y++) for (int z = 0; z < 1024; z++)
	{
		Set( 6, y, z, 255 );
		Set( 1017, y, z, 255 );
	}
	for (int x = 0; x < 1024; x++) for (int z = 0; z < 1024; z++)
	{
		Set( x, 6, z, 255 );
		Set( x, 257, z, 255 );
	}
	for (int x = 0; x < 1024; x++) for (int y = 0; y < 256; y++)
	{
		Set( x, y, 6, 255 );
		Set( x, y, 1017, 255 );
	}
	// performance note: these coords prevent exessive brick traversal.
}

// World::Clear
// ----------------------------------------------------------------------------
void World::Clear()
{
#if 1
	// easiest top just clear the top-level grid and recycle all bricks
	memset( grid, 0, GRIDWIDTH * GRIDHEIGHT * GRIDDEPTH * sizeof( uint ) );
	memset( trash, 0, BRICKCOUNT * 4 );
	for (uint i = 0; i < BRICKCOUNT; i++) trash[(i * 31 /* prevent false sharing*/) & (BRICKCOUNT - 1)] = i;
	ClearMarks();
#else
	for (int z = 0; z < GRIDDEPTH; z++) for (int y = 0; y < GRIDHEIGHT; y++) for (int x = 0; x < GRIDWIDTH; x++)
	{
		const uint cellIdx = x + z * GRIDWIDTH + y * GRIDWIDTH * GRIDDEPTH;
		const uint g = grid[cellIdx];
		if (g & 1)
		{
		#if THREADSAFEWORLD
			const uint trashItem = InterlockedAdd( &trashHead, 31 ) - 31;
			trash[trashItem & (BRICKCOUNT - 1)] = g >> 1;
		#else
			trash[trashHead++ & (BRICKCOUNT - 1)] = g >> 1;
		#endif
		}
	}
	memset( grid, 0, GRIDWIDTH * GRIDHEIGHT * GRIDDEPTH * sizeof( uint ) );
#endif
}

// World::ScrollX
// ----------------------------------------------------------------------------
void World::ScrollX( const int offset )
{
	if (offset % BRICKDIM != 0) FatalError( "ScollX( %i ):\nCan only scroll by multiples of %i.", offset, BRICKDIM );
	const int o = abs( offset / BRICKDIM );
	for (uint z = 0; z < GRIDDEPTH; z++) for (uint y = 0; y < GRIDHEIGHT; y++)
	{
		uint* line = grid + z * GRIDWIDTH + y * GRIDWIDTH * GRIDDEPTH;
		uint backup[GRIDWIDTH];
		if (offset < 0)
		{
			for (int x = 0; x < o; x++) backup[x] = line[x];
			for (int x = 0; x < GRIDWIDTH - o; x++) line[x] = line[x + o];
			for (int x = 0; x < o; x++) line[GRIDWIDTH - o + x] = backup[x];
		}
		else
		{
			for (int x = 0; x < o; x++) backup[x] = line[GRIDWIDTH - o + x];
			for (int x = GRIDWIDTH - 1; x >= o; x--) line[x] = line[x - o];
			for (int x = 0; x < o; x++) line[x] = backup[x];
		}
	}
}

// World::ScrollX
// ----------------------------------------------------------------------------
void World::ScrollY( const int offset )
{
	if (offset % BRICKDIM != 0) FatalError( "ScollY( %i ):\nCan only scroll by multiples of %i.", offset, BRICKDIM );
	// TODO
}

// World::ScrollX
// ----------------------------------------------------------------------------
void World::ScrollZ( const int offset )
{
	if (offset % BRICKDIM != 0) FatalError( "ScollZ( %i ):\nCan only scroll by multiples of %i.", offset, BRICKDIM );
	// TODO
}

// World::LoadSky
// ----------------------------------------------------------------------------
void World::LoadSky( const char* filename, const char* bin_name )
{
	// attempt to load skydome from binary file
	ifstream f( bin_name, ios::binary );
	float* pixels = 0;
	if (f.is_open())
	{
		printf( "loading cached hdr data... " );
		f.read( (char*)&skySize.x, sizeof( skySize.x ) );
		f.read( (char*)&skySize.y, sizeof( skySize.y ) );
		// TODO: Mmap
		pixels = (float*)MALLOC64( skySize.x * skySize.y * sizeof( float ) * 3 );
		f.read( (char*)pixels, sizeof( float ) * 3 * skySize.x * skySize.y );
	}
	if (!pixels)
	{
	#if 0
		// load skydome from original .hdr file
		printf( "loading original hdr data... " );
		FREE_IMAGE_FORMAT fif = FIF_UNKNOWN;
		fif = FreeImage_GetFileType( filename, 0 );
		if (fif == FIF_UNKNOWN) fif = FreeImage_GetFIFFromFilename( filename );
		FIBITMAP* dib = FreeImage_Load( fif, filename );
		if (!dib) return;
		skySize.x = FreeImage_GetWidth( dib );
		skySize.y = FreeImage_GetHeight( dib );
		uint pitch = FreeImage_GetPitch( dib );
		uint bpp = FreeImage_GetBPP( dib );
		printf( "Skydome %dx%d, pitch %d @%dbpp\n", skySize.x, skySize.y, pitch, bpp );
		pixels = (float*)MALLOC64( skySize.x * skySize.y * sizeof( float ) * 3 );
		for (int y = 0; y < skySize.y; y++)
		{
			uint* src = (uint*)FreeImage_GetScanLine( dib, skySize.y - 1 - y );
			float* dst = (float*)pixels + y * skySize.x * 3;
			if (bpp == 96) memcpy( dst, src, skySize.x * sizeof( float ) * 3 ); else
				if (bpp == 128) for (int x = 0; x < skySize.x; x++)
					memcpy( (float*)dst + 3 * x, (float*)src + 4 * x * sizeof( float ), sizeof( float ) * 4 );
				else
					FATALERROR( "Reading a skydome with %dbpp is not implemented!", bpp );
		}
		FreeImage_Unload( dib );
		// save skydome to binary file, .hdr is slow to load
		ofstream f( bin_name, ios::binary );
		f.write( (char*)&skySize.x, sizeof( skySize.x ) );
		f.write( (char*)&skySize.y, sizeof( skySize.y ) );
		f.write( (char*)pixels, sizeof( float ) * 3 * skySize.x * skySize.y );
	#endif
	}
	// convert to float4
	float4* pixel4 = new float4[skySize.x * skySize.y];
	for (int y = 0; y < skySize.y; y++) for (int x = 0; x < skySize.x; x++)
		pixel4[x + y * skySize.x] = make_float4(
			pixels[x * 3 + y * 3 * skySize.x],
			pixels[x * 3 + 1 + y * 3 * skySize.x],
			pixels[x * 3 + 2 + y * 3 * skySize.x],
			1
		);
	FREE64( pixels );
	// load a sky dome
	sky = new Buffer( skySize.x * skySize.y * 4, Buffer::READONLY, pixel4 );
	sky->CopyToDevice();
}

/*
Some primitives for drawing in the 3D voxel world.
Note that these rely on World::Set to actually set/reset voxels.
*/

// World::Sphere
// ----------------------------------------------------------------------------
void World::Sphere( const float x, const float y, const float z, const float r, const uint c )
{
	const uint x1 = (uint)max( 0.f, x - r ), x2 = (uint)min( MAPWIDTH - 1.f, x + r + 1 );
	const uint y1 = (uint)max( 0.f, y - r ), y2 = (uint)min( MAPHEIGHT - 1.f, y + r + 1 );
	const uint z1 = (uint)max( 0.f, z - r ), z2 = (uint)min( MAPDEPTH - 1.f, z + r + 1 );
	if (x1 >= MAPWIDTH || y1 >= MAPHEIGHT || z1 >= MAPDEPTH ||
		x2 >= MAPWIDTH || y2 >= MAPHEIGHT || z2 >= MAPDEPTH) return;
	const float r2 = r * r, m2 = (r - 2) * (r - 2);
	for (uint u = x1; u < x2; u++) for (uint v = y1; v < y2; v++) for (uint w = z1; w < z2; w++)
	{
		float d2 = SQR( (float)u - x ) + SQR( (float)v - y ) + SQR( (float)w - z );
		if (d2 < r2 /* && d2 > m2 */) Set( u, v, w, c );
	}
}

// World::HDisc
// ----------------------------------------------------------------------------
void World::HDisc( const float x, const float y, const float z, const float r, const uint c )
{
	const uint x1 = (uint)max( 0.f, x - r ), x2 = (uint)min( MAPWIDTH - 1.f, x + r + 1 );
	const uint z1 = (uint)max( 0.f, z - r ), z2 = (uint)min( MAPDEPTH - 1.f, z + r + 1 );
	if (x1 >= MAPWIDTH || y >= MAPHEIGHT || z1 >= MAPDEPTH ||
		x2 >= MAPWIDTH || z2 >= MAPDEPTH) return;
	const float r2 = r * r;
	for (uint u = x1; u < x2; u++) for (uint w = z1; w < z2; w++)
	{
		float d2 = SQR( (float)u - x ) + SQR( (float)w - z );
		if (d2 < r2) Set( u, (uint)y, w, c );
	}
}

// World::Print
// ----------------------------------------------------------------------------
void World::Print( const char* text, const uint x, const uint y, const uint z, const uint c )
{
	static char f[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz1234567890+-=/\\*:;()[]{}<>!?.,'\"&#$_%^|";
	static int* r = 0;
	if (!r)
	{
		r = new int[256];
		memset( r, 0, 1024 );
		for (int i = 0; i < strlen( f ); i++) r[f[i]] = i;
	}
	for (int i = 0; i < strlen( text ); i++) if (text[i] > 32)
	{
		const int t = r[text[i]], cx = t % 13, cy = t / 13;
		uint* a = font->buffer + cx * 6 + cy * 10 * 78;
		for (int v = 0; v < 10; v++) for (int u = 0; u < 6; u++)
			if ((a[u + v * 78] & 0xffffff) == 0) Set( x + u + i * 6, y + (9 - v), z, c );
	}
}

// World::LoadSprite
// ----------------------------------------------------------------------------
uint World::LoadSprite( const char* voxFile, bool palShift )
{
	// attempt to load the .vox file
	FILE* file = fopen( voxFile, "rb" );
	if (!file) FatalError( "LoadSprite( %s ):\nFile does not exist.", voxFile );
	static struct ChunkHeader { char name[4]; int N; union { int M; char mainName[4]; }; } header;
	fread( &header, 1, sizeof( ChunkHeader ), file );
	if (strncmp( header.name, "VOX ", 4 )) FatalError( "LoadSprite( %s ):\nBad header.", voxFile );
	if (header.N != 150) FatalError( "LoadSprite( %s ):\nBad version (%i).", voxFile, header.N );
	if (strncmp( header.mainName, "MAIN", 4 )) FatalError( "LoadSprite( %s ):\nNo MAIN chunk.", voxFile );
	fread( &header.N, 1, 4, file ); // eat MAIN chunk num bytes of chunk content (N)
	fread( &header.N, 1, 4, file ); // eat MAIN chunk num bytes of children chunks (M)
	// initialize the palette to the default palette
	static uint palette[256];
	static uint default_palette[256] = {
		0x00000000, 0xffffffff, 0xffccffff, 0xff99ffff, 0xff66ffff, 0xff33ffff, 0xff00ffff, 0xffffccff, 0xffccccff, 0xff99ccff, 0xff66ccff, 0xff33ccff, 0xff00ccff, 0xffff99ff, 0xffcc99ff, 0xff9999ff,
		0xff6699ff, 0xff3399ff, 0xff0099ff, 0xffff66ff, 0xffcc66ff, 0xff9966ff, 0xff6666ff, 0xff3366ff, 0xff0066ff, 0xffff33ff, 0xffcc33ff, 0xff9933ff, 0xff6633ff, 0xff3333ff, 0xff0033ff, 0xffff00ff,
		0xffcc00ff, 0xff9900ff, 0xff6600ff, 0xff3300ff, 0xff0000ff, 0xffffffcc, 0xffccffcc, 0xff99ffcc, 0xff66ffcc, 0xff33ffcc, 0xff00ffcc, 0xffffcccc, 0xffcccccc, 0xff99cccc, 0xff66cccc, 0xff33cccc,
		0xff00cccc, 0xffff99cc, 0xffcc99cc, 0xff9999cc, 0xff6699cc, 0xff3399cc, 0xff0099cc, 0xffff66cc, 0xffcc66cc, 0xff9966cc, 0xff6666cc, 0xff3366cc, 0xff0066cc, 0xffff33cc, 0xffcc33cc, 0xff9933cc,
		0xff6633cc, 0xff3333cc, 0xff0033cc, 0xffff00cc, 0xffcc00cc, 0xff9900cc, 0xff6600cc, 0xff3300cc, 0xff0000cc, 0xffffff99, 0xffccff99, 0xff99ff99, 0xff66ff99, 0xff33ff99, 0xff00ff99, 0xffffcc99,
		0xffcccc99, 0xff99cc99, 0xff66cc99, 0xff33cc99, 0xff00cc99, 0xffff9999, 0xffcc9999, 0xff999999, 0xff669999, 0xff339999, 0xff009999, 0xffff6699, 0xffcc6699, 0xff996699, 0xff666699, 0xff336699,
		0xff006699, 0xffff3399, 0xffcc3399, 0xff993399, 0xff663399, 0xff333399, 0xff003399, 0xffff0099, 0xffcc0099, 0xff990099, 0xff660099, 0xff330099, 0xff000099, 0xffffff66, 0xffccff66, 0xff99ff66,
		0xff66ff66, 0xff33ff66, 0xff00ff66, 0xffffcc66, 0xffcccc66, 0xff99cc66, 0xff66cc66, 0xff33cc66, 0xff00cc66, 0xffff9966, 0xffcc9966, 0xff999966, 0xff669966, 0xff339966, 0xff009966, 0xffff6666,
		0xffcc6666, 0xff996666, 0xff666666, 0xff336666, 0xff006666, 0xffff3366, 0xffcc3366, 0xff993366, 0xff663366, 0xff333366, 0xff003366, 0xffff0066, 0xffcc0066, 0xff990066, 0xff660066, 0xff330066,
		0xff000066, 0xffffff33, 0xffccff33, 0xff99ff33, 0xff66ff33, 0xff33ff33, 0xff00ff33, 0xffffcc33, 0xffcccc33, 0xff99cc33, 0xff66cc33, 0xff33cc33, 0xff00cc33, 0xffff9933, 0xffcc9933, 0xff999933,
		0xff669933, 0xff339933, 0xff009933, 0xffff6633, 0xffcc6633, 0xff996633, 0xff666633, 0xff336633, 0xff006633, 0xffff3333, 0xffcc3333, 0xff993333, 0xff663333, 0xff333333, 0xff003333, 0xffff0033,
		0xffcc0033, 0xff990033, 0xff660033, 0xff330033, 0xff000033, 0xffffff00, 0xffccff00, 0xff99ff00, 0xff66ff00, 0xff33ff00, 0xff00ff00, 0xffffcc00, 0xffcccc00, 0xff99cc00, 0xff66cc00, 0xff33cc00,
		0xff00cc00, 0xffff9900, 0xffcc9900, 0xff999900, 0xff669900, 0xff339900, 0xff009900, 0xffff6600, 0xffcc6600, 0xff996600, 0xff666600, 0xff336600, 0xff006600, 0xffff3300, 0xffcc3300, 0xff993300,
		0xff663300, 0xff333300, 0xff003300, 0xffff0000, 0xffcc0000, 0xff990000, 0xff660000, 0xff330000, 0xff0000ee, 0xff0000dd, 0xff0000bb, 0xff0000aa, 0xff000088, 0xff000077, 0xff000055, 0xff000044,
		0xff000022, 0xff000011, 0xff00ee00, 0xff00dd00, 0xff00bb00, 0xff00aa00, 0xff008800, 0xff007700, 0xff005500, 0xff004400, 0xff002200, 0xff001100, 0xffee0000, 0xffdd0000, 0xffbb0000, 0xffaa0000,
		0xff880000, 0xff770000, 0xff550000, 0xff440000, 0xff220000, 0xff110000, 0xffeeeeee, 0xffdddddd, 0xffbbbbbb, 0xffaaaaaa, 0xff888888, 0xff777777, 0xff555555, 0xff444444, 0xff222222, 0xff111111
	};
	memcpy( palette, default_palette, 1024 );
	// create the sprite
	Sprite* newSprite = new Sprite();
	SpriteFrame* frame = 0;
	int frameCount = 1; // will be overwritten if we encounter a 'PACK' chunk
	// load chunks
	while (1)
	{
		memset( &header, 0, sizeof( ChunkHeader ) );
		fread( &header, 1, sizeof( ChunkHeader ), file );
		if (feof( file ) || header.name[0] == 0) break; // assume end of file
		else if (!strncmp( header.name, "PACK", 4 ))
		{
			fread( &frameCount, 1, 4, file );
		}
		else if (!strncmp( header.name, "SIZE", 4 ))
		{
			if (!frame) frame = new SpriteFrame();
			fread( &frame->size, 1, 12, file );
			swap( frame->size.y, frame->size.z ); // sorry Magica, z=up is just wrong
		}
		else if (!strncmp( header.name, "XYZI", 4 ))
		{
			struct { uchar x, y, z, i; } xyzi;
			uint N;
			int3 s = frame->size;
			fread( &N, 1, 4, file );
			frame->buffer = new unsigned char[s.x * s.y * s.z];
			memset( frame->buffer, 0, s.x * s.y * s.z );
			for (uint i = 0; i < N; i++)
			{
				fread( &xyzi, 1, 4, file );
				frame->buffer[xyzi.x + xyzi.z * s.x + xyzi.y * s.x * s.y] = xyzi.i;
			}
			if (newSprite->frame.size() == frameCount) FatalError( "LoadSprite( %s ):\nBad frame count.", voxFile );
			newSprite->frame.push_back( frame );
			frame = 0;
		}
		else if (!strncmp( header.name, "RGBA", 4 ))
		{
			fread( palette, 4, 256, file );
		}
		else if (!strncmp( header.name, "MATT", 4 ))
		{
			int dummy[8192]; // we are not supporting materials for now.
			fread( dummy, 1, header.N, file );
		}
		else break; // FatalError( "LoadSprite( %s ):\nUnknown chunk.", voxFile );
	}
	fclose( file );
	// finalize new sprite
	int3 maxSize = make_int3( 0 );
	const uint offset =  palShift ? 1 : 0;
	for (int i = 0; i < frameCount; i++)
	{
		SpriteFrame* frame = newSprite->frame[i];
		for (int s = frame->size.x * frame->size.y * frame->size.z, i = 0; i < s; i++) if (frame->buffer[i])
		{
			const uint c = palette[frame->buffer[i] - offset];
			const uint blue = min( 3u, (((c >> 16) & 255) + 31) >> 6 );
			const uint green = min( 7u, (((c >> 8) & 255) + 15) >> 5 );
			const uint red = min( 7u, ((c & 255) + 15) >> 5 );
			const uint p = (red << 5) + (green << 2) + blue;
			frame->buffer[i] = (p == 0) ? (1 << 5) : p;
		}
		maxSize.x = max( maxSize.x, frame->size.x );
		maxSize.y = max( maxSize.y, frame->size.y );
		maxSize.z = max( maxSize.z, frame->size.z );
	}
	// create the backup frame for sprite movement
	SpriteFrame* backupFrame = new SpriteFrame();
	backupFrame->size = maxSize;
	backupFrame->buffer = new uchar[maxSize.x * maxSize.y * maxSize.z];
	newSprite->backup = backupFrame;
	sprite.push_back( newSprite );
	// all done, return sprite index
	return (uint)sprite.size() - 1;
}

// World::CloneSprite
// ----------------------------------------------------------------------------
uint World::CloneSprite( const uint idx )
{
	// clone frame data, wich contain pointers to shared frame data
	Sprite* newSprite = new Sprite();
	if (idx >= sprite.size()) return 0;
	newSprite->frame = sprite[idx]->frame;
	newSprite->hasShadow = sprite[idx]->hasShadow;
	// clone backup frame, which will be unique per instance
	SpriteFrame* backupFrame = new SpriteFrame();
	backupFrame->size = sprite[idx]->backup->size;
	backupFrame->buffer = new uchar[backupFrame->size.x * backupFrame->size.y * backupFrame->size.z];
	newSprite->backup = backupFrame;
	sprite.push_back( newSprite );
	return (uint)sprite.size() - 1;
}

// World::CreateSprite
// ----------------------------------------------------------------------------
uint World::CreateSprite( const int3 pos, const int3 size, const int frames )
{
	// create a sprite from voxel information the world
	Sprite* newSprite = new Sprite();
	uint voxelsPerFrame = size.x * size.y * size.z;
	for (int i = 0; i < frames; i++)
	{
		SpriteFrame* frame = new SpriteFrame();
		frame->size = size;
		frame->buffer = new unsigned char[voxelsPerFrame];
		
		uint bx = (pos.x / BRICKDIM) & (GRIDWIDTH - 1);
		uint by = (pos.y / BRICKDIM) & (GRIDHEIGHT - 1);
		uint bz = (pos.z / BRICKDIM) & (GRIDDEPTH - 1);
		int sbx = bx, sby = by, sbz = bz;
		uint cellIdx = bx + bz * GRIDWIDTH + by * GRIDWIDTH * GRIDDEPTH;
		uint lx = pos.x & (BRICKDIM - 1), ly = pos.y & (BRICKDIM - 1), lz = pos.z & (BRICKDIM - 1);
		uint brickIdx = lx + ly * BRICKDIM + lz * BRICKDIM * BRICKDIM;

		uint slx = lx, sly = ly;

		int dx = pos.x % BRICKDIM;
		int dy = pos.y % BRICKDIM;
		int dz = pos.z % BRICKDIM;

		int odx = dx, ody = dy, odz = dz;

		int mx = ((pos.x / BRICKDIM) & (GRIDWIDTH - 1));
		int my = ((pos.y / BRICKDIM) & (GRIDHEIGHT - 1));
		int mz = ((pos.z / BRICKDIM) & (GRIDDEPTH - 1));

		int omx = mx, omy = my, omz = mz;

		for (int z = 0; z < size.z; z++)
		{
			int o2Idx = cellIdx;
			for (int y = 0; y < size.y; y++)
			{
				int oIdx = cellIdx;
				for (int x = 0; x < size.x; x++)
				{
					uint g = grid[cellIdx];

					uint v = ((g & 1) == 0) ? g >> 1: brick[(g >> 1) * BRICKSIZE + brickIdx];
					brickIdx++;
					frame->buffer[x + y * size.x + z * size.x * size.y] = v;
					if (++dx == BRICKDIM)
					{
						dx = 0;
						cellIdx++;
						if (++mx == GRIDWIDTH)
						{
							cellIdx -= GRIDWIDTH;
							mx = 0;
						}
						dirtyGrid[cellIdx] = true;
					}
					if (++lx == BRICKDIM)
					{
						brickIdx -= BRICKDIM;
						lx = 0;
					}
				}

				cellIdx = oIdx;
				dx = odx;
				mx = omx;

				if (++dy == BRICKDIM)
				{
					dy = 0;
					cellIdx += GRIDWIDTH * GRIDDEPTH;
					if (++my == GRIDDEPTH)
					{
						cellIdx -= GRIDWIDTH * GRIDHEIGHT * GRIDDEPTH;
						my = 0;
					}
					oIdx = cellIdx;
				}
				dirtyGrid[cellIdx] = true;
				brickIdx += BRICKDIM - (lx - slx);
				lx = slx;
				if (++ly == BRICKDIM)
				{
					brickIdx -= BRICKDIM * BRICKDIM;
					ly = 0;
				}
			}

			dy = ody;
			my = omy;
			cellIdx = o2Idx;

			if (++dz == BRICKDIM)
			{
				dz = 0;
				cellIdx += GRIDWIDTH;
				if (++mz == GRIDDEPTH)
				{
					cellIdx -= GRIDWIDTH * GRIDDEPTH;
					mz = 0;
				}
				o2Idx = cellIdx;
			}
			dirtyGrid[cellIdx] = true;
			brickIdx += BRICKDIM * BRICKDIM - (ly - sly) * BRICKDIM;
			ly = sly;
			if (++lz == BRICKDIM)
			{
				brickIdx -= BRICKDIM * BRICKDIM * BRICKDIM;
				lz = 0;
			}
		}
		newSprite->frame.push_back( frame );
	}
	// create the backup frame for sprite movement
	SpriteFrame* backupFrame = new SpriteFrame();
	backupFrame->size = size;
	backupFrame->buffer = new uchar[voxelsPerFrame];
	newSprite->backup = backupFrame;
	sprite.push_back( newSprite );
	// all done, return sprite index
	return (uint)sprite.size() - 1;
}

// World::SpriteFrameCount
// ----------------------------------------------------------------------------
uint World::SpriteFrameCount( const uint idx )
{
	// out of bounds checks
	if (idx >= sprite.size()) return 0;
	// return frame count
	return (uint)sprite[idx]->frame.size();
}

// World::EraseSprite (private; called from World::Commit)
// ----------------------------------------------------------------------------
void World::EraseSprite( const uint idx )
{
	// restore pixels occupied by sprite at previous location
	const int3 lastPos = sprite[idx]->lastPos;
	if (lastPos.x == -9999) return;
	const SpriteFrame* backup = sprite[idx]->backup;
	const int3 s = backup->size;

	uint bx = lastPos.x >> 3;
	uint by = lastPos.y >> 3;
	uint bz = lastPos.z >> 3;

	uint cellIdx = bx + (bz << 7) + (by << 14);

	uint dx = lastPos.x & 7;
	uint dy = lastPos.y & 7;
	uint dz = lastPos.z & 7;


	for (int i = 0, w = 0; w < s.z; w++)
	{
		for (int v = 0; v < s.y; v++)
		{
			for (int u = 0; u < s.x; u++, i++)
			{
				Set(lastPos.x + u, lastPos.y + v, lastPos.z + w, backup->buffer[i]);
			}
		}
	}
}

// World::DrawSprite (private; called from World::Commit)
// ----------------------------------------------------------------------------
_declspec(noinline) void World::DrawSprite( const uint idx )
{
	// draw sprite at new location
	const int3& pos = sprite[idx]->currPos;
	if (pos.x != -9999)
	{
		const SpriteFrame* frame = sprite[idx]->frame[sprite[idx]->currFrame];
		SpriteFrame* backup = sprite[idx]->backup;

		const int3& s = backup->size = frame->size;

		uchar* frameBuffer = frame->buffer;
		uchar* backupBuffer = backup->buffer;

		uint bx = (pos.x / BRICKDIM) & (GRIDWIDTH - 1);
		uint by = (pos.y / BRICKDIM) & (GRIDHEIGHT - 1);
		uint bz = (pos.z / BRICKDIM) & (GRIDDEPTH - 1);
		uint sbx = bx, sby = by, sbz = bz;
		uint cellIdx = bx + bz * GRIDWIDTH + by * GRIDWIDTH * GRIDDEPTH;
		uint lx = pos.x & (BRICKDIM - 1), ly = pos.y & (BRICKDIM - 1), lz = pos.z & (BRICKDIM - 1);
		uint brickIdx = lx + ly * BRICKDIM + lz * BRICKDIM * BRICKDIM;

		uint slx = lx, sly = ly;

		uint dx = pos.x % BRICKDIM;
		uint dy = pos.y % BRICKDIM;
		uint dz = pos.z % BRICKDIM;

		uint odx = dx, ody = dy, odz = dz;

		int i = 0;

		uint mx = ((pos.x / BRICKDIM) & (GRIDWIDTH - 1));
		uint my = ((pos.y / BRICKDIM) & (GRIDHEIGHT - 1));
		uint mz = ((pos.z / BRICKDIM) & (GRIDDEPTH - 1));

		uint omx = mx, omy = my, omz = mz;
		uint g = grid[cellIdx];
		uint gshift = g >> 1;
		for (int w = 0; w < s.z; w++)
		{
			int o2Idx = cellIdx;
			for (int v = 0; v < s.y; v++)
			{
				int oIdx = cellIdx;
				for (int u = 0; u < s.x; u++)
				{
					const uint voxel = frameBuffer[i];

					
					
					backupBuffer[i] = ((g&1) == 0) ? gshift : brick[gshift * BRICKSIZE + brickIdx];
					brickIdx++;
					if (voxel != 0)
						Set(pos.x + u, pos.y + v, pos.z + w, voxel);
					i++;
					
					if (++dx == BRICKDIM)
					{
						dx = 0;
						cellIdx++;						
						if (++mx == GRIDWIDTH)
						{
							cellIdx -= GRIDWIDTH;
							mx = 0;
						}
						brickIdx -= BRICKDIM;
						g = grid[cellIdx];
						gshift = g >> 1;
					}
				}
				brickIdx += BRICKDIM - (dx - slx);
				cellIdx = oIdx;
				dx = odx;
				mx = omx;
				
				if (++dy == BRICKDIM)
				{
					dy = 0;
					cellIdx += GRIDWIDTH * GRIDDEPTH;
					if (++my == GRIDDEPTH)
					{
						cellIdx -= GRIDWIDTH * GRIDHEIGHT * GRIDDEPTH;
						my = 0;
					}
					oIdx = cellIdx;
				}
				g = grid[cellIdx];
				gshift = g >> 1;
				
				if (++ly == BRICKDIM)
				{
					brickIdx -= BRICKDIM * BRICKDIM;
					ly = 0;
				}
			}
			brickIdx += BRICKDIM * BRICKDIM - (ly - sly) * BRICKDIM;
			ly = sly;
			dy = ody;
			my = omy;
			cellIdx = o2Idx;

			if (++dz == BRICKDIM)
			{
				dz = 0;
				cellIdx += GRIDWIDTH;
				if (++mz == GRIDDEPTH)
				{
					cellIdx -= GRIDWIDTH * GRIDDEPTH;
					mz = 0;
				}
				o2Idx = cellIdx;
				brickIdx -= BRICKDIM * BRICKDIM * BRICKDIM;
			}
			g = grid[cellIdx];
			gshift = g >> 1;
			
			if (++lz == BRICKDIM)
			{
				
				lz = 0;
			}
			if (lz != dz)
			{
				printf("test");
			}

		}
	}
	// store this location so we can remove the sprite later
	sprite[idx]->lastPos = sprite[idx]->currPos;
}

// World::DrawSpriteShadow
// ----------------------------------------------------------------------------
void World::DrawSpriteShadow( const uint idx )
{
	const int3& pos = sprite[idx]->currPos;
	int x0 = pos.x - 12, x1 = pos.x + 12;
	int z0 = pos.z - 12, z1 = pos.z + 12;
	sprite[idx]->shadowVoxels = 0;
	if (!sprite[idx]->preShadow) sprite[idx]->preShadow = new uint4[1024];
	for (int z = z0; z < z1; z++) for (int x = x0; x < x1; x++)
	{
		// shadow outline
		int d2 = (z - pos.z) * (z - pos.z) * 3 + (x - pos.x) * (x - pos.x) * 2;
		if (d2 < 360)
		{
			// shadow intensity
			int i = (d2 / 3) + 150 + 16 * ((x + z) & 1);
			// find height
			for (int v, y = pos.y; y >= 0; y--) if (v = Read( x, y, z ))
			{
				sprite[idx]->preShadow[sprite[idx]->shadowVoxels++] = make_uint4( x, y, z, v );
				int r = (((v >> 5) & 7) * i) >> 8;
				int g = (((v >> 2) & 7) * i) >> 8;
				int b = ((v & 3) * i) >> 8;
				Plot( x, y, z, max( 1, (r << 5) + (g << 2) + b ) );
				break;
			}
		}
	}
}

// World::RemoveSpriteShadow
// ----------------------------------------------------------------------------
void World::RemoveSpriteShadow( const uint idx )
{
	// restore voxels affected by shadow
	for (uint i = 0; i < sprite[idx]->shadowVoxels; i++)
	{
		uint4 s = sprite[idx]->preShadow[i];
		Plot( s.x, s.y, s.z, s.w );
	}
}

// World::MoveSpriteTo
// ----------------------------------------------------------------------------
void World::MoveSpriteTo( const uint idx, const uint x, const uint y, const uint z )
{
	// out of bounds checks
	if (idx >= sprite.size()) return;
	// set new sprite location and frame
	if (sprite[idx]->currPos.x != x || sprite[idx]->currPos.y != y || sprite[idx]->currPos.z != z)
	{
		sprite[idx]->currPos = make_int3(x, y, z);
		sprite[idx]->draw = true;
		sprite[idx]->updated = true;
		//sprite[idx]->remove = true;
	}
}

// World::RemoveSprite
// ----------------------------------------------------------------------------
void World::RemoveSprite( const uint idx )
{
	// move sprite to -9999 to keep it from taking CPU cycles
	sprite[idx]->currPos.x = -9999;
	sprite[idx]->remove = true;
	//sprite[idx]->draw = false;
}

// World::SetSpriteFrame
// ----------------------------------------------------------------------------
void World::SetSpriteFrame( const uint idx, const uint frame )
{
	if (idx >= sprite.size()) return;
	if (frame >= sprite[idx]->frame.size()) return;
	sprite[idx]->currFrame = frame;
}

// World::SpriteHit
// ----------------------------------------------------------------------------
bool World::SpriteHit( const uint A, const uint B )
{
	// get the bounding boxes for the two sprites
	int3 b1A = sprite[A]->currPos, b1B = sprite[B]->currPos;
	if (b1A.x == -9999 || b1B.x == -9999) return false; // early out: at least one is inactive
	SpriteFrame* frameA = sprite[A]->frame[sprite[A]->currFrame];
	SpriteFrame* frameB = sprite[B]->frame[sprite[B]->currFrame];
	int3 b2A = b1A + frameA->size, b2B = b1B + frameB->size;
	// get the intersection of the bounds
	int3 b1 = make_int3( max( b1A.x, b1B.x ), max( b1A.y, b1B.y ), max( b1A.z, b1B.z ) );
	int3 b2 = make_int3( min( b2A.x, b2B.x ), min( b2A.y, b2B.y ), min( b2A.z, b2B.z ) );
	if (b1.x > b2.x || b1.y > b2.y || b1.z > b2.z) return false;
	// check individual voxels
	int3 pA = b1 - b1A, pB = b1 - b1B;
	int3 sizeA = frameA->size, sizeB = frameB->size, sizeI = b2 - b1;
	uchar* dA = frameA->buffer + pA.x + pA.y * sizeA.x + pA.z * sizeA.x * sizeA.y;
	uchar* dB = frameB->buffer + pB.x + pB.y * sizeB.x + pB.z * sizeB.x * sizeB.y;
	for (int z = 0; z < sizeI.z; z++)
	{
		for (int y = 0; y < sizeI.y; y++)
		{
			for (int x = 0; x < sizeI.x; x++) if (dA[x] * dB[x]) return true;
			dA += sizeA.x, dB += sizeB.x;
		}
		dA += sizeA.x * sizeA.y, dB += sizeB.x * sizeB.y;
	}
	return false;
}

// World::CreateParticles
// ----------------------------------------------------------------------------
uint World::CreateParticles( const uint count )
{
	Particles* p = new Particles( count );
	particles.push_back( p );
	return (uint)(particles.size() - 1);
}

// World::SetParticle
// ----------------------------------------------------------------------------
void World::SetParticle( const uint set, const uint idx, const uint3 pos, const uint v )
{
	particles[set]->voxel[idx] = make_uint4( pos, v );
}

// World::DrawParticles
// ----------------------------------------------------------------------------
void World::DrawParticles( const uint set )
{
	if (particles[set]->count == 0 || particles[set]->voxel[0].x == -9999) return; // inactive
	
	for (uint s = particles[set]->count, i = 0; i < s; i++)
	{
		const uint4 v = particles[set]->voxel[i];
		const uint bx = (v.x / BRICKDIM) & (GRIDWIDTH - 1);
		const uint by = (v.y / BRICKDIM) & (GRIDHEIGHT - 1);
		const uint bz = (v.z / BRICKDIM) & (GRIDDEPTH - 1);
		const uint cellIdx = bx + bz * GRIDWIDTH + by * GRIDWIDTH * GRIDDEPTH;
		const uint lx = v.x & (BRICKDIM - 1), ly = v.y & (BRICKDIM - 1), lz = v.z & (BRICKDIM - 1);
		const uint brickIdx = lx + ly * BRICKDIM + lz * BRICKDIM * BRICKDIM;

		particles[set]->backup[i] = make_uint4( v.x, v.y, v.z, Get( cellIdx, brickIdx ) );
		Set( v.x, v.y, v.z, v.w );
	}
}

// World::EraseParticles
// ----------------------------------------------------------------------------
void World::EraseParticles( const uint set )
{
	for (int s = (int)particles[set]->count, i = s - 1; i >= 0; i--)
	{
		const uint4 v = particles[set]->backup[i];
		Set( v.x, v.y, v.z, v.w );
	}
}

// World::LoadTile
// ----------------------------------------------------------------------------
uint World::LoadTile( const char* voxFile )
{
	tile.push_back( new Tile( voxFile ) );
	return (uint)tile.size() - 1;
}

// World::DrawTile / DrawTileVoxels
// ----------------------------------------------------------------------------
void World::DrawTile( const uint idx, const uint x, const uint y, const uint z )
{
	if (x >= GRIDWIDTH || y >= GRIDHEIGHT || z > GRIDDEPTH) return;
	const uint cellIdx = x + z * GRIDWIDTH + y * GRIDWIDTH * GRIDDEPTH;
	DrawTileVoxels( cellIdx, tile[idx]->voxels, tile[idx]->zeroes );
}
void World::DrawTileVoxels( const uint cellIdx, const uchar* voxels, const uint zeroes )
{
	const uint g = grid[cellIdx];
	uint brickIdx;
	if ((g & 1) == 1) brickIdx = g >> 1; else brickIdx = NewBrick(), grid[cellIdx] = (brickIdx << 1) | 1;
	// copy tile data to brick
	memcpy( brick + brickIdx * BRICKSIZE, voxels, BRICKSIZE );
	Mark( brickIdx );
	brickInfo[brickIdx].zeroes = zeroes;
}

// World::DrawTiles
// ----------------------------------------------------------------------------
void World::DrawTiles( const char* tileString, const uint x, const uint y, const uint z )
{
	for (uint s = (uint)strlen( tileString ), i = 0; i < s; i++)
	{
		const char t = tileString[i];
		DrawTile( tileString[i] - '0', x + i, y, z );
	}
}

// World::LoadBigTile
// ----------------------------------------------------------------------------
uint World::LoadBigTile( const char* voxFile )
{
	bigTile.push_back( new BigTile( voxFile ) );
	return (uint)bigTile.size() - 1;
}

// World::DrawBigTile
// ----------------------------------------------------------------------------
void World::DrawBigTile( const uint idx, const uint x, const uint y, const uint z )
{
	if (x >= GRIDWIDTH / 2 || y >= GRIDHEIGHT / 2 || z > GRIDDEPTH / 2) return;
	const uint cellIdx = x * 2 + z * 2 * GRIDWIDTH + y * 2 * GRIDWIDTH * GRIDDEPTH;
	DrawTileVoxels( cellIdx, bigTile[idx]->tile[0].voxels, bigTile[idx]->tile[0].zeroes );
	DrawTileVoxels( cellIdx + 1, bigTile[idx]->tile[1].voxels, bigTile[idx]->tile[1].zeroes );
	DrawTileVoxels( cellIdx + GRIDWIDTH * GRIDDEPTH, bigTile[idx]->tile[2].voxels, bigTile[idx]->tile[2].zeroes );
	DrawTileVoxels( cellIdx + GRIDWIDTH * GRIDDEPTH + 1, bigTile[idx]->tile[3].voxels, bigTile[idx]->tile[3].zeroes );
	DrawTileVoxels( cellIdx + GRIDWIDTH, bigTile[idx]->tile[4].voxels, bigTile[idx]->tile[4].zeroes );
	DrawTileVoxels( cellIdx + GRIDWIDTH + 1, bigTile[idx]->tile[5].voxels, bigTile[idx]->tile[5].zeroes );
	DrawTileVoxels( cellIdx + GRIDWIDTH + GRIDWIDTH * GRIDDEPTH, bigTile[idx]->tile[6].voxels, bigTile[idx]->tile[6].zeroes );
	DrawTileVoxels( cellIdx + GRIDWIDTH + GRIDWIDTH * GRIDDEPTH + 1, bigTile[idx]->tile[7].voxels, bigTile[idx]->tile[7].zeroes );
}

// World::DrawBigTiles
// ----------------------------------------------------------------------------
void World::DrawBigTiles( const char* tileString, const uint x, const uint y, const uint z )
{
	for (uint s = (uint)strlen( tileString ), i = 0; i < s; i++)
	{
		const char t = tileString[i];
		if (t != ' ') DrawBigTile( tileString[i] - '0', x + i, y, z );
	}
}

// World::Trace
// ----------------------------------------------------------------------------
float4 FixZeroDeltas( float4 V )
{
	if (fabs( V.x ) < 1e-8f) V.x = V.x < 0 ? -1e-8f : 1e-8f;
	if (fabs( V.y ) < 1e-8f) V.y = V.y < 0 ? -1e-8f : 1e-8f;
	if (fabs( V.z ) < 1e-8f) V.z = V.z < 0 ? -1e-8f : 1e-8f;
	return V;
}
uint World::TraceRay( float4 A, const float4 B, float& dist, float3& N, int steps )
{
	const float4 V = FixZeroDeltas( B ), rV = make_float4( 1 / V.x, 1 / V.y, 1 / V.z, 1 );
	const bool originOutsideGrid = A.x < 0 || A.y < 0 || A.z < 0 || A.x > MAPWIDTH || A.y > MAPHEIGHT || A.z > MAPDEPTH;
	if (steps == 999999 && originOutsideGrid)
	{
		// use slab test to clip ray origin against scene AABB
		const float tx1 = -A.x * rV.x, tx2 = (MAPWIDTH - A.x) * rV.x;
		float tmin = min( tx1, tx2 ), tmax = max( tx1, tx2 );
		const float ty1 = -A.y * rV.y, ty2 = (MAPHEIGHT - A.y) * rV.y;
		tmin = max( tmin, min( ty1, ty2 ) ), tmax = min( tmax, max( ty1, ty2 ) );
		const float tz1 = -A.z * rV.z, tz2 = (MAPDEPTH - A.z) * rV.z;
		tmin = max( tmin, min( tz1, tz2 ) ), tmax = min( tmax, max( tz1, tz2 ) );
		if (tmax < tmin || tmax <= 0) return 0; /* ray misses scene */ else A += tmin * V; // new ray entry point
	}
	uint4 pos = make_uint4( clamp( (int)A.x, 0, MAPWIDTH - 1 ), clamp( (int)A.y, 0, MAPHEIGHT - 1 ), clamp( (int)A.z, 0, MAPDEPTH - 1 ), 0 );
	const int bits = SELECT( 4, 34, V.x > 0 ) + SELECT( 3072, 10752, V.y > 0 ) + SELECT( 1310720, 3276800, V.z > 0 ); // magic
	float tmx = ((float)((pos.x & BPMX) + ((bits >> (5 - BDIMLOG2)) & (1 << BDIMLOG2))) - A.x) * rV.x;
	float tmy = ((float)((pos.y & BPMY) + ((bits >> (13 - BDIMLOG2)) & (1 << BDIMLOG2))) - A.y) * rV.y;
	float tmz = ((float)((pos.z & BPMZ) + ((bits >> (21 - BDIMLOG2)) & (1 << BDIMLOG2))) - A.z) * rV.z, t = 0;
	const float tdx = (float)DIR_X * rV.x, tdy = (float)DIR_Y * rV.y, tdz = (float)DIR_Z * rV.z;
	uint last = 0;
	while (true)
	{
		// check main grid
		const uint o = grid[pos.x / BRICKDIM + (pos.z / BRICKDIM) * GRIDWIDTH + (pos.y / BRICKDIM) * GRIDWIDTH * GRIDDEPTH];
		if (o != 0) if ((o & 1) == 0) /* solid */
		{
			dist = t, N = make_float3( (float)((last == 0) * DIR_X), (float)((last == 1) * DIR_Y), (float)((last == 2) * DIR_Z) ) * -1.0f;
			return o >> 1;
		}
		else // brick
		{
			const float4 I = A + V * t;
			uint p = (clamp( (uint)I.x, pos.x & BPMX, (pos.x & BPMX) + BMSK ) << 20) +
				(clamp( (uint)I.y, pos.y & BPMY, (pos.y & BPMY) + BMSK ) << 10) +
				clamp( (uint)I.z, pos.z & BPMZ, (pos.z & BPMZ) + BMSK );
			const uint pn = p & TOPMASK3;
			float dmx = ((float)((p >> 20) + OFFS_X) - A.x) * rV.x;
			float dmy = ((float)(((p >> 10) & 1023) + OFFS_Y) - A.y) * rV.y;
			float dmz = ((float)((p & 1023) + OFFS_Z) - A.z) * rV.z, d = t;
			do
			{
				const uint idx = (o >> 1) * BRICKSIZE + ((p >> 20) & BMSK) + ((p >> 10) & BMSK) * BRICKDIM + (p & BMSK) * BDIM2;
				const unsigned int color = brick[idx];
				if (color != 0U)
				{
					dist = d, N = make_float3( (float)((last == 0) * DIR_X), (float)((last == 1) * DIR_Y), (float)((last == 2) * DIR_Z) ) * -1.0f;
					return color;
				}
				d = min( dmx, min( dmy, dmz ) );
				if (d == dmx) dmx += tdx, p += DIR_X << 20, last = 0;
				if (d == dmy) dmy += tdy, p += DIR_Y << 10, last = 1;
				if (d == dmz) dmz += tdz, p += DIR_Z, last = 2;
			} while ((p & TOPMASK3) == pn);
		}
		if (!--steps) break;
		t = min( tmx, min( tmy, tmz ) );
		if (t == tmx) tmx += tdx * BRICKDIM, pos.x += DIR_X * BRICKDIM, last = 0;
		if (t == tmy) tmy += tdy * BRICKDIM, pos.y += DIR_Y * BRICKDIM, last = 1;
		if (t == tmz) tmz += tdz * BRICKDIM, pos.z += DIR_Z * BRICKDIM, last = 2;
		if ((pos.x & (65536 - MAPWIDTH)) + (pos.y & (65536 - MAPWIDTH)) + (pos.z & (65536 - MAPWIDTH))) break;
	}
	return 0U;
}

// World::TracePacket
// ----------------------------------------------------------------------------
void World::TracePacket( const float3 O, const float3 P1, const float3 P2, const float3 P3, const float3 P4, RayPacket& rp )
{
	// Ray Tracing Animated Scenes using Coherent Grid Traversal, Wald et al., 2006
	// 0. Generate tile rays, TODO: postpone until actually needed?
	float3 D[TILESIZE2], rD[TILESIZE2];
	uint b[TILESIZE2];
	for (int i = 0, y = 0; y < TILESIZE; y++) for (int x = 0; x < TILESIZE; x++, i++)
	{
		D[i] = (P1 + (P2 - P1) * ((float)x / (TILESIZE - 1)) + (P4 - P1) * ((float)y / (TILESIZE - 1))) - O;
		rD[i] = make_float3( 1.0f / D[i].x, 1.0f / D[i].y, 1.0f / D[i].z );
		rp.c[i] = 0, rp.Nt[i] = make_float4( 1e34f );
		b[i] = SELECT( 4, 34, D[i].x > 0 ) + SELECT( 3072, 10752, D[i].y > 0 ) + SELECT( 1310720, 3276800, D[i].z > 0 ); // magic
	}
	const float3 gO = O * (1.0f / BRICKDIM);
	// 1. Find dominant axis
	float3 D1 = P1 - O, D2 = P2 - O, D3 = P3 - O, D4 = P4 - O;
	const uint k = dominantAxis( D1 );
	if (k == 0)
	{
		// x is major axis; u = y and v = z.
		D1 *= 1.0f / D1.x, D2 *= 1.0f / D2.x, D3 *= 1.0f / D3.x, D4 *= 1.0f / D4.x;
		const float4 delta = make_float4(
			min( min( D1.y, D2.y ), min( D3.y, D4.y ) ), max( max( D1.y, D2.y ), max( D3.y, D4.y ) ),
			min( min( D1.z, D2.z ), min( D3.z, D4.z ) ), max( max( D1.z, D2.z ), max( D3.z, D4.z ) )
		);
		// start stepping
		int w = (int)O.x / 8, dir = D1.x > 0 ? 1 : -1;
		float4 frustum = (make_float4( O.y, O.y, O.z, O.z ) - delta * (O.x - (float)w * 8)) * (1.0f / BRICKDIM);
		int4 uvcells1 = make_int4( frustum );
		while (1)
		{
			frustum += delta;
			const int4 uvcells2 = make_int4( frustum );
			const int4 uvcells = make_int4( max( min( uvcells1.x, uvcells2.x ), 0 ),
				min( max( uvcells1.y, uvcells2.y ), GRIDHEIGHT - 1 ),
				max( min( uvcells1.z, uvcells2.z ), 0 ),
				min( max( uvcells1.w, uvcells2.w ), GRIDDEPTH - 1 ) );
			uvcells1 = uvcells2;
			for (int v = uvcells.z; v <= uvcells.w; v++) for (int u = uvcells.x; u <= uvcells.y; u++)
			{
				const uint g = grid[w + v * GRIDWIDTH + u * GRIDWIDTH * GRIDDEPTH];
				if (!g) continue;
				for (int i = 0; i < TILESIZE2; i++)
				{
					const int bits = b[i];
					const float tdx = (float)DIR_X * rD[i].x, tdy = (float)DIR_Y * rD[i].y, tdz = (float)DIR_Z * rD[i].z;
					uint last = 0;
					// TODO: we can probably just test three planes based on 'bits'?
					float tmin = ((float)w - gO.x) * rD[i].x, tmax = tmin + rD[i].x; // TODO: careful, only works for x+
					const float ty1 = ((float)u - gO.y) * rD[i].y, ty2 = (((float)u + 1) - gO.y) * rD[i].y;
					tmin = max( tmin, min( ty1, ty2 ) ), tmax = min( tmax, max( ty1, ty2 ) );
					const float tz1 = ((float)v - gO.z) * rD[i].z, tz2 = (((float)v + 1) - gO.z) * rD[i].z;
					tmin = max( tmin, min( tz1, tz2 ) ), tmax = min( tmax, max( tz1, tz2 ) );
					if (tmax <= tmin) continue;
					float t = BRICKDIM * max( 0.0f, tmin );
					if ((g & 1) == 0)
					{
						if (t < rp.Nt[i].w) // solid supervoxel; finalize ray
						{
							rp.Nt[i] = make_float4( 0, 1, 0, t ); // TODO: determine actual normal
							rp.c[i] = g >> 1;
						}
					}
					else
					{
						// traverse brick
						const float3 E = O + t * D[i]; // this is where the ray enters the voxel
						uint p = (clamp( (uint)E.x, (uint)w * BRICKDIM, (uint)w * BRICKDIM + BRICKDIM - 1 ) << 20) +
							(clamp( (uint)E.y, (uint)u * BRICKDIM, (uint)u * BRICKDIM + BRICKDIM - 1 ) << 10) +
							clamp( (uint)E.z, (uint)v * BRICKDIM, (uint)v * BRICKDIM + BRICKDIM - 1 );
						const uint pn = p & TOPMASK3;
						float dmx = ((float)((p >> 20) + OFFS_X) - O.x) * rD[i].x;
						float dmy = ((float)(((p >> 10) & 1023) + OFFS_Y) - O.y) * rD[i].y;
						float dmz = ((float)((p & 1023) + OFFS_Z) - O.z) * rD[i].z, d = t;
						do
						{
							const uint idx = (g >> 1) * BRICKSIZE + ((p >> 20) & BMSK) + ((p >> 10) & BMSK) * BRICKDIM + (p & BMSK) * BDIM2;
							const unsigned int c = brick[idx];
							if (c != 0U && d < rp.Nt[i].w)
							{
								rp.Nt[i] = make_float4( make_float3( (float)((last == 0) * DIR_X), (float)((last == 1) * DIR_Y), (float)((last == 2) * DIR_Z) ) * -1.0f, d );
								rp.c[i] = c;
								break;
							}
							d = min( dmx, min( dmy, dmz ) );
							if (d == dmx) dmx += tdx, p += DIR_X << 20, last = 0;
							if (d == dmy) dmy += tdy, p += DIR_Y << 10, last = 1;
							if (d == dmz) dmz += tdz, p += DIR_Z, last = 2;
						} while ((p & TOPMASK3) == pn);
					}
				}
			}
			int i = 0;
			for (i = 0; i < TILESIZE2; i++) if (rp.Nt[i].w == 1e34f) break;
			if (i == TILESIZE2) break;
			w += dir;
			if (w < 0 || w >= GRIDWIDTH) break;
		}
	}
}

/*
Render flow:
1. GLFW application loop in template.cpp calls World::Render:
   - If changes were made to the scene during the previous frame,
	 these will now be committed (moved in vram to their final location).
   - The render kernel is invoked. It will run in the background.
	 The render kernel stores final pixels in an OpenGL texture.
   - Sprites (if any) are removed from the world, so Game::Tick operates as
	 if their voxels do not exist.
2. GLFW application loop calls Game::Tick:
   - Game::Tick executes game logic on the CPU.
   - Via the world object, changes are placed in the commit buffer.
3. GLFW application loop calls World::Commit:
   - Sprites are added back, to be displayed.
   - The new top-level grid (128^3 uints, 8MB) is copied into the commit buffer.
   - Changed bricks are detected and added to the commit buffer.
   - An asynchronous copy of the commit buffer (host->device) is started.
4. GLFW application loop draws a full-screen quad, using the texture
   filled by the render kernel.
The Game::Tick CPU code and the asynchronous copy of the commit buffer are
typically hidden completely behind the OpenCL render kernel.
In the next frame, the commit kernel synchronizes with this copy,
to ensure it completes before the commit kernel is executed on this data.
*/

// World::Render
// ----------------------------------------------------------------------------
void World::Render()
{
	// prepare for rendering
	const mat4 M = camMat;
	const float aspectRatio = (float)SCRWIDTH / SCRHEIGHT;
	params.E = TransformPosition( make_float3( 0 ), M );
	params.oneOverRes = make_float2( 1.0f / SCRWIDTH, 1.0f / SCRHEIGHT );
	params.p0 = TransformPosition( make_float3( -aspectRatio, 1, 3 ), M );
	params.p1 = TransformPosition( make_float3( aspectRatio, 1, 3 ), M );
	params.p2 = TransformPosition( make_float3( -aspectRatio, -1, 3 ), M );
	params.R0 = RandomUInt();
	static uint frame = 0;
	params.frame = frame++;
#if CPUONLY == 0
	// default path: use GPU to render current world state in the background
	if (copyInFlight)
	{
		// finalize the asynchronous copy by executing the commit kernel
		if (tasks > 0)
		{
			committer->SetArgument( 0, (int)tasks );
			committer->Run( (tasks + 63) & (65536 - 32), 4, &copyDone, &commitDone );
		}
		copyInFlight = false, commitInFlight = true;
	}
	// get render parameters to GPU and invoke kernel asynchronously
	paramBuffer->CopyToDevice( false );
	renderer->Run( screen, make_int2( 32, 4 ) );
#else
	// CPU-only path
	static Surface* target = 0;
	if (!target) target = new Surface( SCRWIDTH, SCRHEIGHT );
#if 0
	// reference flow, single ray traversal - WARNING: SLOW
#pragma omp parallel for schedule (dynamic)
	for (int y = 0; y < SCRHEIGHT; y++)
	{
		float2 uv;
		uv.y = (float)y / SCRHEIGHT;
		for (int x = 0; x < SCRWIDTH; x++)
		{
			// create primary ray
			uv.x = (float)x / SCRWIDTH;
			const float3 P = params.p0 + (params.p1 - params.p0) * uv.x + (params.p2 - params.p0) * uv.y;
			const float3 D = normalize( P - params.E );
			// trace primary ray
			float dist;
			float3 N;
			const uint voxel = TraceRay( make_float4( params.E, 1 ), make_float4( D, 1 ), dist, N, 999999 /* no cap needed */ );
			// visualize result
			float3 pixel;
			if (voxel == 0) pixel = make_float3( 0.5f, 0.5f, 1.0f ); /* sky, for now */ else
			{
				const float3 BRDF1 = INVPI * make_float3( (float)(voxel >> 5) * (1 / 7.0f), (float)((voxel >> 2) & 7) * (1 / 7.0f), (float)(voxel & 3) * (1 / 3.0f) );
				// hardcoded lights - image based lighting, no visibility test
				pixel = BRDF1 * 2 * (
					(N.x * N.x) * ((-N.x + 1) * make_float3( NX0 ) + (N.x + 1) * make_float3( NX1 )) +
					(N.y * N.y) * ((-N.y + 1) * make_float3( NY0 ) + (N.y + 1) * make_float3( NY1 )) +
					(N.z * N.z) * ((-N.z + 1) * make_float3( NZ0 ) + (N.z + 1) * make_float3( NZ1 ))
				);
			}
			const int r = (int)(sqrtf( clamp( pixel.x, 0.0f, 1.0f ) ) * 255.0f);
			const int g = (int)(sqrtf( clamp( pixel.y, 0.0f, 1.0f ) ) * 255.0f);
			const int b = (int)(sqrtf( clamp( pixel.z, 0.0f, 1.0f ) ) * 255.0f);
			target->buffer[x + y * SCRWIDTH] = r + (g << 8) + (b << 16);
		}
	}
#else
	// start worker threads
	// TODO
	// tile / packet renderer
	const uint tileCount = (SCRHEIGHT / TILESIZE) * (SCRWIDTH / TILESIZE);
	Timer timer;
	timer.reset();
#pragma omp parallel for schedule (dynamic)
	for (int t = 0; t < tileCount; t++)
	{
		int x = (t % (SCRWIDTH / TILESIZE)) * TILESIZE;
		int y = (t / (SCRWIDTH / TILESIZE)) * TILESIZE;
		// create primary ray
		const float2 uv0 = make_float2( (float)x / SCRWIDTH, (float)y / SCRHEIGHT );
		const float2 uv1 = make_float2( (float)(x + (TILESIZE - 1)) / SCRWIDTH, (float)(y + (TILESIZE - 1)) / SCRHEIGHT );
		const float3 P1 = params.p0 + (params.p1 - params.p0) * uv0.x + (params.p2 - params.p0) * uv0.y;
		const float3 P2 = params.p0 + (params.p1 - params.p0) * uv1.x + (params.p2 - params.p0) * uv0.y;
		const float3 P3 = params.p0 + (params.p1 - params.p0) * uv1.x + (params.p2 - params.p0) * uv1.y;
		const float3 P4 = params.p0 + (params.p1 - params.p0) * uv0.x + (params.p2 - params.p0) * uv1.y;
		RayPacket rp;
		TracePacket( params.E, P1, P2, P3, P4, rp );
		// visualize result
		for (int v = 0; v < TILESIZE; v++) for (int u = 0; u < TILESIZE; u++)
		{
			const uint voxel = rp.c[u + v * TILESIZE];
			const float4 Nt = rp.Nt[u + v * TILESIZE];
			// visualize result
			float3 pixel;
			if (voxel == 0) pixel = make_float3( 0.5f, 0.5f, 1.0f ); /* sky, for now */ else
			{
				const float3 BRDF1 = INVPI * make_float3( (float)(voxel >> 5) * (1 / 7.0f), (float)((voxel >> 2) & 7) * (1 / 7.0f), (float)(voxel & 3) * (1 / 3.0f) );
				// hardcoded lights - image based lighting, no visibility test
				pixel = BRDF1 * 2 * (
					(Nt.x * Nt.x) * ((-Nt.x + 1) * make_float3( NX0 ) + (Nt.x + 1) * make_float3( NX1 )) +
					(Nt.y * Nt.y) * ((-Nt.y + 1) * make_float3( NY0 ) + (Nt.y + 1) * make_float3( NY1 )) +
					(Nt.z * Nt.z) * ((-Nt.z + 1) * make_float3( NZ0 ) + (Nt.z + 1) * make_float3( NZ1 ))
				);
			}
			const int r = (int)(sqrtf( clamp( pixel.x, 0.0f, 1.0f ) ) * 255.0f);
			const int g = (int)(sqrtf( clamp( pixel.y, 0.0f, 1.0f ) ) * 255.0f);
			const int b = (int)(sqrtf( clamp( pixel.z, 0.0f, 1.0f ) ) * 255.0f);
			target->buffer[x + u + (y + v) * SCRWIDTH] = r + (g << 8) + (b << 16);
		}
	}
	static float frameTime = 42.3f; // estimate
	frameTime = 0.95f * frameTime + 50.0f * timer.elapsed();
	printf( "render time: %5.2fms\n", frameTime );
#endif
	glBindTexture( GL_TEXTURE_2D, targetTextureID );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, SCRWIDTH, SCRHEIGHT, 0, GL_RGBA, GL_UNSIGNED_BYTE, target->buffer );
	glBindTexture( GL_TEXTURE_2D, 0 );
	CheckGL();
#endif
}

// World::Commit
// ----------------------------------------------------------------------------



void World::Commit()
{
	// add the sprites and particles to the world
	for (int s = (int)sprite.size(), i = 0; i < s; i++)
	{
		if (sprite[i]->hasShadow)
			DrawSpriteShadow(i);
		DrawSprite(i);
	}
	for (int s = (int)particles.size(), i = 0; i < s; i++) DrawParticles( i );
	// make sure the previous commit completed
	if (commitInFlight) clWaitForEvents( 1, &commitDone );
	// replace the initial commit buffer by a double-sized buffer in pinned memory
	static uint* pinnedMemPtr = 0;
	if (pinnedMemPtr == 0)
	{
		const uint pinnedSize = commitSize + gridSize;
		cl_mem pinned = clCreateBuffer( Kernel::GetContext(), CL_MEM_WRITE_ONLY | CL_MEM_ALLOC_HOST_PTR, commitSize * 2, 0, 0 );
		pinnedMemPtr = (uint*)clEnqueueMapBuffer( Kernel::GetQueue(), pinned, 1, CL_MAP_WRITE, 0, pinnedSize, 0, 0, 0, 0 );
		StreamCopy( (__m256i*)(pinnedMemPtr + commitSize / 4), (__m256i*)grid, gridSize );
		grid = pinnedMemPtr + commitSize / 4; // top-level grid resides at the start of the commit buffer
	}
	// gather changed bricks
	tasks = 0;
	uint* brickIndices = pinnedMemPtr + gridSize / 4;
	uchar* changedBricks = (uchar*)(brickIndices + MAXCOMMITS);
	for (uint j = 0; j < BRICKCOUNT / 32; j++) if (IsDirty32( j ) /* if not dirty: skip 32 bits at once */)
	{
		for (uint k = 0; k < 32; k++)
		{
			const uint i = j * 32 + k;
			if (!IsDirty( i )) continue;
			*brickIndices++ = i; // store index of modified brick at start of commit buffer
			StreamCopy( (__m256i*)changedBricks, (__m256i*)(brick + i * BRICKSIZE), BRICKSIZE );
			changedBricks += BRICKSIZE, tasks++;
		}
		ClearMarks32( j );
		if (tasks + 32 >= MAXCOMMITS) break; // we have too many commits; postpone
	}
	// copy top-level grid to start of pinned buffer in preparation of final transfer
	if (tasks > 0 || firstFrame) StreamCopyMT( (__m256i*)pinnedMemPtr, (__m256i*)grid, gridSize );
	// asynchroneously copy the CPU data to the GPU via the commit buffer
	if (tasks > 0 || firstFrame)
	{
		// enqueue (on queue 2) memcopy of pinned buffer to staging buffer on GPU
		const uint copySize = firstFrame ? commitSize : (gridSize + MAXCOMMITS * 4 + tasks * BRICKSIZE);
		clEnqueueWriteBuffer( Kernel::GetQueue2(), devmem, 0, 0, copySize, pinnedMemPtr, 0, 0, 0 );
		// enqueue (on queue 2) vram-to-vram copy of the top-level grid to a 3D OpenCL image buffer
		size_t origin[3] = { 0, 0, 0 };
		size_t region[3] = { GRIDWIDTH, GRIDDEPTH, GRIDHEIGHT };
		clEnqueueCopyBufferToImage( Kernel::GetQueue2(), devmem, gridMap, 0, origin, region, 0, 0, &copyDone );
		copyInFlight = true;	// next render should wait for this commit to complete
		firstFrame = false;		// next frame is not the first frame
	}
	
	// bricks and top-level grid have been moved to the final host-side commit buffer; remove sprites and particles
	// NOTE: this must explicitly happen in reverse order.
	
	
	for (int s = (int)particles.size(), i = s - 1; i >= 0; i--) EraseParticles(i);
	for (int s = (int)sprite.size(), i = s - 1; i >= 0; i--)
	{
		EraseSprite(i);
		if (sprite[i]->hasShadow) RemoveSpriteShadow(i);
	}
	
}

// World::StreamCopyMT
// ----------------------------------------------------------------------------
#define COPYTHREADS	4
void World::StreamCopyMT( __m256i* dst, __m256i* src, const uint bytes )
{
	// fast copying of large 32-byte aligned / multiple of 32 sized data blocks:
	// streaming __m256 read/writes, on multiple threads
	assert( (bytes & 31) == 0 );
	int N = (int)(bytes / 32);
	static JobManager* jm = JobManager::GetJobManager();
	static CopyJob cj[COPYTHREADS];
	__m256i* s = src;
	__m256i* d = dst;
	for (int i = 0; i < (COPYTHREADS - 1); i++, s += N / COPYTHREADS, d += N / COPYTHREADS)
	{
		cj[i].dst = d, cj[i].src = s, cj[i].N = N / COPYTHREADS;
		jm->AddJob2( &cj[i] );
	}
	cj[COPYTHREADS - 1].dst = d;
	cj[COPYTHREADS - 1].src = s;
	cj[COPYTHREADS - 1].N = N - (COPYTHREADS - 1) * (N / COPYTHREADS);
	jm->AddJob2( &cj[COPYTHREADS - 1] );
	jm->RunJobs();
}

// Tile::Tile
// ----------------------------------------------------------------------------
Tile::Tile( const char* voxFile )
{
	// abuse the sprite loader to get the tile in memory
	uint spriteIdx = GetWorld()->LoadSprite( voxFile );
	Sprite* sprite = GetWorld()->sprite[spriteIdx];
	// make some assumptions
	if (sprite->frame.size() != 1) FatalError( "LoadTile( \"%s\" ):\nExpected a single frame in the tile file.", voxFile );
	SpriteFrame* frame = sprite->frame[0];
	if (frame->size.x == BRICKDIM * 2) FatalError( "LoadTile( \"%s\" ):\nExpected an 8x8x8 tile; use LoadBigTile for 16x16x16 tiles.", voxFile );
	if (frame->size.x != BRICKDIM) FatalError( "LoadTile( \"%s\" ):\nExpected an 8x8x8 tile.", voxFile );
	// convert data
	uint zeroCount = 0;
	for (int i = 0; i < BRICKSIZE; i++)
	{
		uchar v = frame->buffer[i];
		voxels[i] = v;
		if (v == 0) zeroCount++;
	}
	zeroes = zeroCount;
	// remove the sprite from the world
	GetWorld()->sprite.pop_back();
}

// BigTile::BigTile
// ----------------------------------------------------------------------------
BigTile::BigTile( const char* voxFile )
{
	// abuse the sprite loader to get the tile in memory
	uint spriteIdx = GetWorld()->LoadSprite( voxFile );
	Sprite* sprite = GetWorld()->sprite[spriteIdx];
	// make some assumptions
	if (sprite->frame.size() != 1) FatalError( "LoadTile( \"%s\" ):\nExpected a single frame in the tile file.", voxFile );
	SpriteFrame* frame = sprite->frame[0];
	if (frame->size.x == BRICKDIM) FatalError( "LoadTile( \"%s\" ):\nExpected a 16x16x16 tile; use LoadTile for 8x8x8 tiles.", voxFile );
	if (frame->size.x != BRICKDIM * 2) FatalError( "LoadTile( \"%s\" ):\nExpected a 16x16x16 tile.", voxFile );
	// convert data: store as 8 bricks for efficient rendering
	for (int subTile = 0; subTile < 8; subTile++)
	{
		int sx = subTile & 1, sy = (subTile >> 1) & 1, sz = (subTile >> 2) & 1;
		uint zeroCount = 0;
		for (int z = 0; z < BRICKDIM; z++) for (int y = 0; y < BRICKDIM; y++) for (int x = 0; x < BRICKDIM; x++)
		{
			uchar v = frame->buffer[sx * BRICKDIM + x + (sy * BRICKDIM + y) * BRICKDIM * 2 + (sz * BRICKDIM + z) * 4 * BRICKDIM * BRICKDIM];
			tile[subTile].voxels[x + y * BRICKDIM + z * BRICKDIM * BRICKDIM] = v;
			if (v == 0) zeroCount++;
		}
		tile[subTile].zeroes = zeroCount;
	}
	// remove the sprite from the world
	GetWorld()->sprite.pop_back();
	delete sprite;
}

// EOF