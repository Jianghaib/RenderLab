#include "SObjSampler.h"

#include "SampleRaster.h"

#include <UI/Hierarchy.h>
#include <UI/Attribute.h>
#include <UI/Setting.h>

#include <CppUtil/Qt/PaintImgOpCreator.h>
#include <CppUtil/Qt/OpThread.h>

#include <CppUtil/Engine/Roamer.h>
#include <CppUtil/Engine/RTX_Renderer.h>
#include <CppUtil/Engine/PathTracer.h>
#include <CppUtil/Engine/Viewer.h>
#include <CppUtil/Engine/Scene.h>
#include <CppUtil/Engine/SObj.h>
#include <CppUtil/Engine/OptixAIDenoiser.h>

#include <CppUtil/Basic/CSV.h>
#include <CppUtil/Basic/Image.h>
#include <CppUtil/Basic/LambdaOp.h>
#include <CppUtil/Basic/OpQueue.h>
#include <CppUtil/Basic/GStorage.h>
#include <CppUtil/Basic/Math.h>

#include <ROOT_PATH.h>

#include <qdebug.h>
#include <qtimer.h>
#include <qfiledialog.h>
#include <qevent.h>

#include <synchapi.h>

using namespace App;
using namespace CppUtil::Qt;
using namespace CppUtil::Engine;
using namespace CppUtil::Basic;
using namespace glm;
using namespace std;
using namespace Ui;

template<>
bool SObjSampler::GetArgAs<bool>(ENUM_ARG arg) const {
	return GetArg(arg).asBool();
}

template<>
long SObjSampler::GetArgAs<long>(ENUM_ARG arg) const {
	return GetArg(arg).asLong();
}

template<>
int SObjSampler::GetArgAs<int>(ENUM_ARG arg) const {
	return static_cast<int>(GetArg(arg).asLong());
}

template<>
string SObjSampler::GetArgAs<string>(ENUM_ARG arg) const {
	return GetArg(arg).asString();
}

template<>
vector<string> SObjSampler::GetArgAs<vector<string>>(ENUM_ARG arg) const {
	return GetArg(arg).asStringList();
}

SObjSampler::~SObjSampler() {
	delete timer;
}

SObjSampler::SObjSampler(const ArgMap & argMap, QWidget *parent, Qt::WindowFlags flags)
	: argMap(argMap), QMainWindow(parent, flags), timer(nullptr)
{
	ui.setupUi(this);

	Init();
}

void SObjSampler::UI_Op(Operation::Ptr op) {
	op->Run();
}

void SObjSampler::Init() {
	InitScene();
	InitRaster();
	InitRTX();
	InitTimer();
}

void SObjSampler::InitScene() {
	bool isNotFromRootPath = GetArgAs<bool>(ENUM_ARG::notrootpath);
	string path = GetArgAs<string>(ENUM_ARG::sobj);
	string prefix = isNotFromRootPath ? "" : ROOT_PATH;

	auto root = SObj::Load(prefix + path);
	scene = ToPtr(new Scene(root, "scene"));
}

void SObjSampler::InitRaster() {
	sampleRaster = ToPtr(new SampleRaster(scene));
	roamer = ToPtr(new Roamer(ui.OGLW_Raster));
	roamer->SetLock(true);

	ui.OGLW_Raster->SetInitOp(ToPtr(new LambdaOp([=]() {
		roamer->Init();
		sampleRaster->Init();
	})));

	auto paintOp = ToPtr(new OpQueue);

	paintOp->Push(ToPtr(new LambdaOp([=]() {
		sampleRaster->Draw();
	})));

	paintOp->Push(ToPtr(new LambdaOp([=]() {
		dataMap[ENUM_TYPE::FRAG_COLOR] = sampleRaster->GetData(SampleRaster::ENUM_TYPE::FRAG_COLOR);
		dataMap[ENUM_TYPE::POSITION] = sampleRaster->GetData(SampleRaster::ENUM_TYPE::POSITION);
		dataMap[ENUM_TYPE::VIEW_DIR] = sampleRaster->GetData(SampleRaster::ENUM_TYPE::VIEW_DIR);
		dataMap[ENUM_TYPE::NORMAL] = sampleRaster->GetData(SampleRaster::ENUM_TYPE::NORMAL);
		dataMap[ENUM_TYPE::MAT_COLOR] = sampleRaster->GetData(SampleRaster::ENUM_TYPE::MAT_COLOR);
		dataMap[ENUM_TYPE::IOR_ROUGHNESS_ID] = sampleRaster->GetData(SampleRaster::ENUM_TYPE::IOR_ROUGHNESS_ID);
	}, false)));

	ui.OGLW_Raster->SetPaintOp(paintOp);
}

void SObjSampler::InitRTX() {
	int maxDepth = GetArgAs<int>(ENUM_ARG::maxdepth);
	auto generator = [=]()->RayTracer::Ptr {
		auto pathTracer = ToPtr(new PathTracer);
		pathTracer->maxDepth = maxDepth;

		return pathTracer;
	};

	PaintImgOpCreator pioc(ui.OGLW_RayTracer);
	paintImgOp = pioc.GenScenePaintOp();
	paintImgOp->SetOp(512, 512);
	auto img = paintImgOp->GetImg();
	rtxRenderer = ToPtr(new RTX_Renderer(generator));
	rtxRenderer->maxLoop = GetArgAs<int>(ENUM_ARG::samplenum);

	drawImgThread = ToPtr(new OpThread([=]() {
		rtxRenderer->Run(scene, img);

		if (!GetArgAs<bool>(ENUM_ARG::notdenoise))
			OptixAIDenoiser::GetInstance().Denoise(img);

		SaveData();

		QApplication::exit();
	}));
	drawImgThread->start();

	printProgressThread = ToPtr(new OpThread([=]() {
		int dotNum = 0;
		while (rtxRenderer->ProgressRate() != 1.f) {
			string dotStr;
			for (int i = 0; i < 6; i++)
				dotStr += i < dotNum % 6 ? "." : " ";
			printf("\rprogress rate : %d%% %s", static_cast<int>(rtxRenderer->ProgressRate() * 100), dotStr.c_str());

			dotNum++;
			Sleep(500);
		}
		printf("\rprogress rate : 100%%\nRender complete!\n\n");
	}));
	printProgressThread->start();
}

void SObjSampler::InitTimer() {
	delete timer;

	timer = new QTimer;
	timer->callOnTimeout([this]() {
		ui.OGLW_Raster->update();
		ui.OGLW_RayTracer->update();
	});

	const size_t fps = 30;
	timer->start(1000 / fps);
}

void SObjSampler::SaveData() {
	static const vector<string> keys = {
		"ID",
		"DirectIllum_R",
		"DirectIllum_G",
		"DirectIllum_B",
		"Position_x",
		"Position_y",
		"Position_z",
		"ViewDir_x",
		"ViewDir_y",
		"ViewDir_z",
		"Normal_x",
		"Normal_y",
		"Normal_z",
		"MatColor_R",
		"MatColor_G",
		"MatColor_B",
		"IOR",
		"Roughness",
		"IndirectIllum_R",
		"IndirectIllum_G",
		"IndirectIllum_B",
	};

	bool isNotFromRootPath = GetArgAs<bool>(ENUM_ARG::notrootpath);
	string path = GetArgAs<string>(ENUM_ARG::csv);
	string prefix = isNotFromRootPath ? "" : ROOT_PATH;

	CSV<float> csv(keys);
	vector<ENUM_TYPE> enumTypes = {
		//ENUM_TYPE::FRAG_COLOR,
		ENUM_TYPE::POSITION,
		ENUM_TYPE::VIEW_DIR,
		ENUM_TYPE::NORMAL,
		ENUM_TYPE::MAT_COLOR,
		//ENUM_TYPE::IOR_ROUGHNESS_ID,
	};

	map<int, string> ID2name;
	for (int row = 0; row < 512; row++) {
		for (int col = 0; col < 512; col++) {
			vector<float> lineVals;
			int idx = (row * 512 + col) * 3;

			float ID = dataMap[ENUM_TYPE::IOR_ROUGHNESS_ID][idx + 2];
			ID2name[ID] = scene->GetName(ID);

			float ior = dataMap[ENUM_TYPE::IOR_ROUGHNESS_ID][idx + 0];
			float roughness = dataMap[ENUM_TYPE::IOR_ROUGHNESS_ID][idx + 1];
			lineVals.push_back(ID);

			vec3 localIllum(
				dataMap[ENUM_TYPE::FRAG_COLOR][idx + 0],
				dataMap[ENUM_TYPE::FRAG_COLOR][idx + 1],
				dataMap[ENUM_TYPE::FRAG_COLOR][idx + 2]
			);
			localIllum = sqrt(localIllum);

			lineVals.push_back(localIllum.r);
			lineVals.push_back(localIllum.g);
			lineVals.push_back(localIllum.b);

			for (auto enumType : enumTypes) {
				for (int channel = 0; channel < 3; channel++)
					lineVals.push_back(dataMap[enumType][idx + channel]);
			}

			vec3 globalIllum = paintImgOp->GetImg()->GetPixel_F(row, col);
			globalIllum = sqrt(globalIllum);

			vec3 indirectIllum = max(globalIllum - localIllum, 0.f);

			lineVals.push_back(ior);
			lineVals.push_back(roughness);

			lineVals.push_back(indirectIllum.x);
			lineVals.push_back(indirectIllum.y);
			lineVals.push_back(indirectIllum.z);

			csv.AddLine(lineVals);
		}
	}

	csv.Save(prefix + path);
	File idMapFile(prefix + path + "_ID_name.txt", File::WRITE);
	for (auto & pair : ID2name)
		idMapFile.Printf("%d : %s\n", pair.first, pair.second);
	idMapFile.Close();
}

const docopt::value & SObjSampler::GetArg(ENUM_ARG arg) const {
	static const docopt::value invalid;

	auto target = argMap.find(arg);
	if (target == argMap.cend())
		return invalid;

	return target->second;
}