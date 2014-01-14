/*
 * Copyright 2013-2014 Dario Manesku. All rights reserved.
 * License: http://www.opensource.org/licenses/BSD-2-Clause
 */

#include <string>
#include <vector>
#include <algorithm>

#include "common.h"

#include <bgfx.h>
#include <bx/timer.h>
#include <bx/readerwriter.h>
#include "entry/entry.h"
#include "fpumath.h"

#define RENDER_SHADOW_PASS_ID 0
#define RENDER_SCENE_PASS_ID  1

uint32_t packUint32(uint8_t _x, uint8_t _y, uint8_t _z, uint8_t _w)
{
	union
	{
		uint32_t ui32;
		uint8_t arr[4];
	} un;

	un.arr[0] = _x;
	un.arr[1] = _y;
	un.arr[2] = _z;
	un.arr[3] = _w;

	return un.ui32;
}

uint32_t packF4u(float _x, float _y = 0.0f, float _z = 0.0f, float _w = 0.0f)
{
	const uint8_t xx = uint8_t(_x*127.0f + 128.0f);
	const uint8_t yy = uint8_t(_y*127.0f + 128.0f);
	const uint8_t zz = uint8_t(_z*127.0f + 128.0f);
	const uint8_t ww = uint8_t(_w*127.0f + 128.0f);
	return packUint32(xx, yy, zz, ww);
}

struct PosNormalVertex
{
	float    m_x;
	float    m_y;
	float    m_z;
	uint32_t m_normal;
};

static const float s_texcoord = 5.0f;
static const uint32_t s_numHPlaneVertices = 4;
static PosNormalVertex s_hplaneVertices[s_numHPlaneVertices] =
{
	{ -1.0f, 0.0f,  1.0f, packF4u(0.0f, 1.0f, 0.0f) },
	{  1.0f, 0.0f,  1.0f, packF4u(0.0f, 1.0f, 0.0f) },
	{ -1.0f, 0.0f, -1.0f, packF4u(0.0f, 1.0f, 0.0f) },
	{  1.0f, 0.0f, -1.0f, packF4u(0.0f, 1.0f, 0.0f) },
};

static const uint32_t s_numPlaneIndices = 6;
static const uint16_t s_planeIndices[s_numPlaneIndices] =
{
	0, 1, 2,
	1, 3, 2,
};

static const char* s_shaderPath = NULL;
static bool s_flipV = false;
static float s_texelHalf = 0.0f;
static bgfx::RenderTargetHandle s_rtShadowMap;
static bgfx::UniformHandle u_shadowMap;

static void shaderFilePath(char* _out, const char* _name)
{
	strcpy(_out, s_shaderPath);
	strcat(_out, _name);
	strcat(_out, ".bin");
}

long int fsize(FILE* _file)
{
	long int pos = ftell(_file);
	fseek(_file, 0L, SEEK_END);
	long int size = ftell(_file);
	fseek(_file, pos, SEEK_SET);
	return size;
}

static const bgfx::Memory* load(const char* _filePath)
{
	FILE* file = fopen(_filePath, "rb");
	if (NULL != file)
	{
		uint32_t size = (uint32_t)fsize(file);
		const bgfx::Memory* mem = bgfx::alloc(size+1);
		size_t ignore = fread(mem->data, 1, size, file);
		BX_UNUSED(ignore);
		fclose(file);
		mem->data[mem->size-1] = '\0';
		return mem;
	}

	return NULL;
}

static const bgfx::Memory* loadShader(const char* _name)
{
	char filePath[512];
	shaderFilePath(filePath, _name);
	return load(filePath);
}

static bgfx::ProgramHandle loadProgram(const char* _vsName, const char* _fsName)
{
	const bgfx::Memory* mem;

	// Load vertex shader.
	mem = loadShader(_vsName);
	bgfx::VertexShaderHandle vsh = bgfx::createVertexShader(mem);

	// Load fragment shader.
	mem = loadShader(_fsName);
	bgfx::FragmentShaderHandle fsh = bgfx::createFragmentShader(mem);

	// Create program from shaders.
	bgfx::ProgramHandle program = bgfx::createProgram(vsh, fsh);

	// We can destroy vertex and fragment shader here since
	// their reference is kept inside bgfx after calling createProgram.
	// Vertex and fragment shader will be destroyed once program is
	// destroyed.
	bgfx::destroyVertexShader(vsh);
	bgfx::destroyFragmentShader(fsh);

	return program;
}

void mtxScaleRotateTranslate(float* _result
							, const float _scaleX
							, const float _scaleY
							, const float _scaleZ
							, const float _rotX
							, const float _rotY
							, const float _rotZ
							, const float _translateX
							, const float _translateY
							, const float _translateZ
							)
{
	float mtxRotateTranslate[16];
	float mtxScale[16];

	mtxRotateXYZ(mtxRotateTranslate, _rotX, _rotY, _rotZ);
	mtxRotateTranslate[12] = _translateX;
	mtxRotateTranslate[13] = _translateY;
	mtxRotateTranslate[14] = _translateZ;

	memset(mtxScale, 0, sizeof(float)*16);
	mtxScale[0]  = _scaleX;
	mtxScale[5]  = _scaleY;
	mtxScale[10] = _scaleZ;
	mtxScale[15] = 1.0f;

	mtxMul(_result, mtxScale, mtxRotateTranslate);
}

struct Aabb
{
	float m_min[3];
	float m_max[3];
};

struct Obb
{
	float m_mtx[16];
};

struct Sphere
{
	float m_center[3];
	float m_radius;
};

struct Primitive
{
	uint32_t m_startIndex;
	uint32_t m_numIndices;
	uint32_t m_startVertex;
	uint32_t m_numVertices;

	Sphere m_sphere;
	Aabb m_aabb;
	Obb m_obb;
};

typedef std::vector<Primitive> PrimitiveArray;

struct Group
{
	Group()
	{
		reset();
	}

	void reset()
	{
		m_vbh.idx = bgfx::invalidHandle;
		m_ibh.idx = bgfx::invalidHandle;
		m_prims.clear();
	}

	bgfx::VertexBufferHandle m_vbh;
	bgfx::IndexBufferHandle m_ibh;
	Sphere m_sphere;
	Aabb m_aabb;
	Obb m_obb;
	PrimitiveArray m_prims;
};
;

struct Mesh
{
	void load(const void* _vertices, uint32_t _numVertices, const bgfx::VertexDecl _decl, const uint16_t* _indices, uint32_t _numIndices)
	{
		Group group;
		const bgfx::Memory* mem;
		uint32_t size;

		size = _numVertices*_decl.getStride();
		mem = bgfx::makeRef(_vertices, size);
		group.m_vbh = bgfx::createVertexBuffer(mem, _decl);

		size = _numIndices*2;
		mem = bgfx::makeRef(_indices, size);
		group.m_ibh = bgfx::createIndexBuffer(mem);

		//TODO:
		// group.m_sphere = ...
		// group.m_aabb = ...
		// group.m_obb = ...
		// group.m_prims = ...

		m_groups.push_back(group);
	}

	void load(const char* _filePath)
	{
#define BGFX_CHUNK_MAGIC_VB BX_MAKEFOURCC('V', 'B', ' ', 0x0)
#define BGFX_CHUNK_MAGIC_IB BX_MAKEFOURCC('I', 'B', ' ', 0x0)
#define BGFX_CHUNK_MAGIC_PRI BX_MAKEFOURCC('P', 'R', 'I', 0x0)

		bx::CrtFileReader reader;
		reader.open(_filePath);

		Group group;

		uint32_t chunk;
		while (4 == bx::read(&reader, chunk) )
		{
			switch (chunk)
			{
			case BGFX_CHUNK_MAGIC_VB:
				{
					bx::read(&reader, group.m_sphere);
					bx::read(&reader, group.m_aabb);
					bx::read(&reader, group.m_obb);

					bx::read(&reader, m_decl);
					uint16_t stride = m_decl.getStride();

					uint16_t numVertices;
					bx::read(&reader, numVertices);
					const bgfx::Memory* mem = bgfx::alloc(numVertices*stride);
					bx::read(&reader, mem->data, mem->size);

					group.m_vbh = bgfx::createVertexBuffer(mem, m_decl);
				}
				break;

			case BGFX_CHUNK_MAGIC_IB:
				{
					uint32_t numIndices;
					bx::read(&reader, numIndices);
					const bgfx::Memory* mem = bgfx::alloc(numIndices*2);
					bx::read(&reader, mem->data, mem->size);
					group.m_ibh = bgfx::createIndexBuffer(mem);
				}
				break;

			case BGFX_CHUNK_MAGIC_PRI:
				{
					uint16_t len;
					bx::read(&reader, len);

					std::string material;
					material.resize(len);
					bx::read(&reader, const_cast<char*>(material.c_str() ), len);

					uint16_t num;
					bx::read(&reader, num);

					for (uint32_t ii = 0; ii < num; ++ii)
					{
						bx::read(&reader, len);

						std::string name;
						name.resize(len);
						bx::read(&reader, const_cast<char*>(name.c_str() ), len);

						Primitive prim;
						bx::read(&reader, prim.m_startIndex);
						bx::read(&reader, prim.m_numIndices);
						bx::read(&reader, prim.m_startVertex);
						bx::read(&reader, prim.m_numVertices);
						bx::read(&reader, prim.m_sphere);
						bx::read(&reader, prim.m_aabb);
						bx::read(&reader, prim.m_obb);

						group.m_prims.push_back(prim);
					}

					m_groups.push_back(group);
					group.reset();
				}
				break;

			default:
				DBG("%08x at %d", chunk, reader.seek() );
				break;
			}
		}

		reader.close();
	}

	void unload()
	{
		for (GroupArray::const_iterator it = m_groups.begin(), itEnd = m_groups.end(); it != itEnd; ++it)
		{
			const Group& group = *it;
			bgfx::destroyVertexBuffer(group.m_vbh);

			if (bgfx::isValid(group.m_ibh) )
			{
				bgfx::destroyIndexBuffer(group.m_ibh);
			}
		}
		m_groups.clear();
	}

	void submit(uint8_t _view, float* _mtx, bgfx::ProgramHandle _program)
	{
		for (GroupArray::const_iterator it = m_groups.begin(), itEnd = m_groups.end(); it != itEnd; ++it)
		{
			const Group& group = *it;

			// Set model matrix for rendering.
			bgfx::setTransform(_mtx);
			bgfx::setProgram(_program);
			bgfx::setIndexBuffer(group.m_ibh);
			bgfx::setVertexBuffer(group.m_vbh);

			// Set shadow map.
			bgfx::setTexture(4, u_shadowMap, s_rtShadowMap);

			// Set render states.
			bgfx::setState(0
					|BGFX_STATE_RGB_WRITE
					|BGFX_STATE_ALPHA_WRITE
					|BGFX_STATE_DEPTH_WRITE
					|BGFX_STATE_DEPTH_TEST_LESS
					|BGFX_STATE_CULL_CCW
					|BGFX_STATE_MSAA
					);

			// Submit primitive for rendering.
			bgfx::submit(_view);
		}
	}

	bgfx::VertexDecl m_decl;
	typedef std::vector<Group> GroupArray;
	GroupArray m_groups;
};

int _main_(int /*_argc*/, char** /*_argv*/)
{
	uint32_t width = 1280;
	uint32_t height = 720;
	uint32_t debug = BGFX_DEBUG_TEXT;
	uint32_t reset = BGFX_RESET_VSYNC;

	bgfx::init();
	bgfx::reset(width, height, reset);

	// Enable debug text.
	bgfx::setDebug(debug);

	// Setup root path for binary shaders. Shader binaries are different
	// for each renderer.
	switch (bgfx::getRendererType() )
	{
	default:
	case bgfx::RendererType::Direct3D9:
		s_shaderPath = "shaders/dx9/";
		s_texelHalf = 0.5f;
		break;

	case bgfx::RendererType::Direct3D11:
		s_shaderPath = "shaders/dx11/";
		break;

	case bgfx::RendererType::OpenGL:
		s_shaderPath = "shaders/glsl/";
		s_flipV = true;
		break;

	case bgfx::RendererType::OpenGLES2:
	case bgfx::RendererType::OpenGLES3:
		s_shaderPath = "shaders/gles/";
		s_flipV = true;
		break;
	}

	// Uniforms.
	u_shadowMap = bgfx::createUniform("u_shadowMap", bgfx::UniformType::Uniform1iv);

	bgfx::UniformHandle u_lightPos = bgfx::createUniform("u_lightPos", bgfx::UniformType::Uniform4fv);
	bgfx::UniformHandle u_lightMtx = bgfx::createUniform("u_lightMtx", bgfx::UniformType::Uniform4x4fv);

	// Programs.
	bgfx::ProgramHandle progPackDepth = loadProgram("vs_smsimple_packdepth", "fs_smsimple_packdepth");
	bgfx::ProgramHandle progDraw      = loadProgram("vs_smsimple_draw",      "fs_smsimple_draw");

	// Vertex declarations.
	bgfx::VertexDecl PosNormalDecl;
	PosNormalDecl.begin();
	PosNormalDecl.add(bgfx::Attrib::Position,  3, bgfx::AttribType::Float);
	PosNormalDecl.add(bgfx::Attrib::Normal,    4, bgfx::AttribType::Uint8, true, true);
	PosNormalDecl.end();

	// Meshes.
	Mesh bunnyMesh;
	Mesh cubeMesh;
	Mesh hollowcubeMesh;
	Mesh hplaneMesh;
	bunnyMesh.load("meshes/bunny.bin");
	cubeMesh.load("meshes/cube.bin");
	hollowcubeMesh.load("meshes/hollowcube.bin");
	hplaneMesh.load(s_hplaneVertices, s_numHPlaneVertices, PosNormalDecl, s_planeIndices, s_numPlaneIndices);

	// Render targets.
	uint16_t shadowMapSize = 512;
	s_rtShadowMap = bgfx::createRenderTarget(shadowMapSize, shadowMapSize, BGFX_RENDER_TARGET_COLOR_RGBA8 | BGFX_RENDER_TARGET_DEPTH_D16);

	// Set view and projection matrices.
	float view[16];
	float proj[16];

	const float eye[3] = { 0.0f, 30.0f, -60.0f };
	const float at[3]  = { 0.0f, 5.0f, 0.0f };
	mtxLookAt(view, eye, at);

	const float aspect = float(int32_t(width) ) / float(int32_t(height) );
	mtxProj(proj, 60.0f, aspect, 0.1f, 1000.0f);

	// Time acumulators.
	float timeAccumulatorLight = 0.0f;
	float timeAccumulatorScene = 0.0f;

	entry::MouseState mouseState;
	while (!entry::processEvents(width, height, debug, reset, &mouseState) )
	{
		// Time.
		int64_t now = bx::getHPCounter();
		static int64_t last = now;
		const int64_t frameTime = now - last;
		last = now;
		const double freq = double(bx::getHPFrequency() );
		const double toMs = 1000.0/freq;
		const float deltaTime = float(frameTime/freq);

		// Update time accumulators.
		timeAccumulatorLight += deltaTime;
		timeAccumulatorScene += deltaTime;

		// Use debug font to print information about this example.
		bgfx::dbgTextClear();
		bgfx::dbgTextPrintf(0, 1, 0x4f, "bgfx/examples/15-shadowmaps-simple");
		bgfx::dbgTextPrintf(0, 2, 0x6f, "Description: Shadow maps example.");
		bgfx::dbgTextPrintf(0, 3, 0x0f, "Frame: % 7.3f[ms]", double(frameTime)*toMs);

		// Setup lights.
		float lightPos[4];
		lightPos[0] = -cos(timeAccumulatorLight);
		lightPos[1] = -1.0f;
		lightPos[2] = -sin(timeAccumulatorLight);
		lightPos[3] = 0.0f;

		bgfx::setUniform(u_lightPos, lightPos);

		// Setup instance matrices.
		float mtxFloor[16];
		mtxScaleRotateTranslate(mtxFloor
			, 30.0f //scaleX
			, 30.0f //scaleY
			, 30.0f //scaleZ
			, 0.0f  //rotX
			, 0.0f  //rotY
			, 0.0f  //rotZ
			, 0.0f  //translateX
			, 0.0f  //translateY
			, 0.0f  //translateZ
			);

		float mtxBunny[16];
		mtxScaleRotateTranslate(mtxBunny
			, 5.0f
			, 5.0f
			, 5.0f
			, 0.0f
			, float(M_PI) - timeAccumulatorScene
			, 0.0f
			, 15.0f
			, 5.0f
			, 0.0f
			);

		float mtxHollowcube[16];
		mtxScaleRotateTranslate(mtxHollowcube
			, 2.5f
			, 2.5f
			, 2.5f
			, 0.0f
			, 1.56f - timeAccumulatorScene
			, 0.0f
			, 0.0f
			, 10.0f
			, 0.0f
			);

		float mtxCube[16];
		mtxScaleRotateTranslate(mtxCube
			, 2.5f
			, 2.5f
			, 2.5f
			, 0.0f
			, 1.56f - timeAccumulatorScene
			, 0.0f
			, -15.0f
			, 5.0f
			, 0.0f
			);

		// Define matrices.
		float screenView[16];
		float screenProj[16];
		mtxIdentity(screenView);
		mtxOrtho(screenProj, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 100.0f);

		float lightView[16];
		float lightProj[16];

		const float eye[3] =
		{
			-lightPos[0],
			-lightPos[1],
			-lightPos[2],
		};
		const float at[3] = { 0.0f, 0.0f, 0.0f };
		mtxLookAt(lightView, eye, at);

		const float area = 30.0f;
		mtxOrtho(lightProj, -area, area, -area, area, -100.0f, 100.0f);

		bgfx::setViewRect(RENDER_SHADOW_PASS_ID, 0, 0, shadowMapSize, shadowMapSize);
		bgfx::setViewRect(RENDER_SCENE_PASS_ID, 0, 0, width, height);

		bgfx::setViewTransform(RENDER_SHADOW_PASS_ID, lightView, lightProj);
		bgfx::setViewTransform(RENDER_SCENE_PASS_ID, view, proj);

		bgfx::setViewRenderTarget(RENDER_SHADOW_PASS_ID, s_rtShadowMap);

		// Clear backbuffer and shadowmap rendertarget at beginning.
		bgfx::setViewClearMask(0x3, BGFX_CLEAR_COLOR_BIT | BGFX_CLEAR_DEPTH_BIT, 0x303030ff, 1.0f, 0);
		bgfx::submitMask(0x3);

		// Render.

		{ // Craft shadow map.

			hplaneMesh.submit(RENDER_SHADOW_PASS_ID, mtxFloor, progPackDepth);
			bunnyMesh.submit(RENDER_SHADOW_PASS_ID, mtxBunny, progPackDepth);
			hollowcubeMesh.submit(RENDER_SHADOW_PASS_ID, mtxHollowcube, progPackDepth);
			cubeMesh.submit(RENDER_SHADOW_PASS_ID, mtxCube, progPackDepth);
		}

		{ // Draw Scene.

			float mtxShadow[16]; //lightviewProjCrop
			float lightMtx[16];  //modelLightviewProjCrop

			const float s = (s_flipV) ? 1.0f : -1.0f; //sign

			const float mtxCrop[16] =
			{
				0.5f,   0.0f, 0.0f, 0.0f,
				0.0f, s*0.5f, 0.0f, 0.0f,
				0.0f,   0.0f, 0.5f, 0.0f,
				0.5f,   0.5f, 0.5f, 1.0f,
			};

			float mtxTmp[16];
			mtxMul(mtxTmp, lightProj, mtxCrop);
			mtxMul(mtxShadow, lightView, mtxTmp);

			// Floor.
			mtxMul(lightMtx, mtxFloor, mtxShadow);
			bgfx::setUniform(u_lightMtx, lightMtx);
			hplaneMesh.submit(RENDER_SCENE_PASS_ID, mtxFloor, progDraw);

			// Bunny.
			mtxMul(lightMtx, mtxBunny, mtxShadow);
			bgfx::setUniform(u_lightMtx, lightMtx);
			bunnyMesh.submit(RENDER_SCENE_PASS_ID, mtxBunny, progDraw);

			// Hollow cube.
			mtxMul(lightMtx, mtxHollowcube, mtxShadow);
			bgfx::setUniform(u_lightMtx, lightMtx);
			hollowcubeMesh.submit(RENDER_SCENE_PASS_ID, mtxHollowcube, progDraw);

			// Cube.
			mtxMul(lightMtx, mtxCube, mtxShadow);
			bgfx::setUniform(u_lightMtx, lightMtx);
			cubeMesh.submit(RENDER_SCENE_PASS_ID, mtxCube, progDraw);
		}

		// Advance to next frame. Rendering thread will be kicked to
		// process submitted rendering primitives.
		bgfx::frame();

	}

	bunnyMesh.unload();
	cubeMesh.unload();
	hollowcubeMesh.unload();
	hplaneMesh.unload();

	bgfx::destroyProgram(progPackDepth);
	bgfx::destroyProgram(progDraw);

	bgfx::destroyRenderTarget(s_rtShadowMap);

	bgfx::destroyUniform(u_shadowMap);
	bgfx::destroyUniform(u_lightPos);
	bgfx::destroyUniform(u_lightMtx);

	// Shutdown bgfx.
	bgfx::shutdown();

	return 0;
}
