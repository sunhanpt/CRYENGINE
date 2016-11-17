// Copyright 2001-2016 Crytek GmbH / Crytek Group. All rights reserved.

#include "StdAfx.h"
#include "Core.h"

#include <CryExtension/CryCreateClassInstance.h>
#include <Schematyc/Env/EnvPackage.h>
#include <Schematyc/Env/Elements/EnvDataType.h>
#include <Schematyc/SerializationUtils/SerializationEnums.inl>
#include <Schematyc/SerializationUtils/SerializationUtils.h>
#include <Schematyc/Utils/StackString.h>

#include "CVars.h"
#include "ObjectPool.h"
#include "Compiler/Compiler.h"
#include "CoreEnv/CoreEnv.h"
#include "Env/EnvRegistry.h"
#include "Runtime/RuntimeRegistry.h"
#include "Script/ScriptRegistry.h"
#include "Script/ScriptView.h"
#include "SerializationUtils/SerializationContext.h"
#include "SerializationUtils/ValidatorArchive.h"
#include "Services/Log.h"
#include "Services/LogRecorder.h"
#include "Services/SettingsManager.h"
#include "Services/TimerSystem.h"
#include "Services/UpdateScheduler.h"
#include "UnitTests/UnitTestRegistrar.h"

#include <CryExtension/ICryPluginManager.h>

namespace Schematyc
{
namespace
{
static void OnLogFileStreamsChange(ICVar* pCVar)
{
	static_cast<CCore&>(GetSchematycCore()).RefreshLogFileStreams();
}

static void OnLogFileMessageTypesChange(ICVar* pCVar)
{
	static_cast<CCore&>(GetSchematycCore()).RefreshLogFileMessageTypes();
}

inline bool WantPrePhysicsUpdate()
{
	return !gEnv->pGameFramework->IsGamePaused() && (gEnv->pSystem->GetSystemGlobalState() == ESYSTEM_GLOBAL_STATE_RUNNING) && !gEnv->IsEditing();
}

inline bool WantUpdate()
{
	return !gEnv->pGameFramework->IsGamePaused() && (gEnv->pSystem->GetSystemGlobalState() == ESYSTEM_GLOBAL_STATE_RUNNING);
}
} // Anonymous

static const char* g_szScriptsFolder = "scripts";
static const char* g_szSettingsFolder = "settings";

class CSchematycCoreCreator : public IGameFrameworkExtensionCreator
{
	CRYINTERFACE_SIMPLE(IGameFrameworkExtensionCreator)

	CRYGENERATE_SINGLETONCLASS(CSchematycCoreCreator, ms_szClassName, 0x0f296b01ff3f4f55, 0xaa10a724a89c73cd)

public:

	virtual ICryUnknown* Create(IGameFramework* pIGameFramework, void* pData) override
	{
		// Create Schematyc core.
		std::shared_ptr<CCore> pSchematycCore;
		if (CryCreateClassInstance(CCore::ms_szClassName, pSchematycCore))
		{
			// Register Schematyc core with game framework
			pIGameFramework->RegisterExtension(pSchematycCore);
			// Ensure registration was successful and cache local pointer.
			GetSchematycCore();
			// Initialize Schematyc core.
			pSchematycCore->Init();
			CryLogAlways("Schematyc core initialized");
			return pSchematycCore.get();
		}
		return nullptr;
	}

public:

	static const char* ms_szClassName;
};

CRYREGISTER_SINGLETON_CLASS(CSchematycCoreCreator)

CSchematycCoreCreator::CSchematycCoreCreator() {}

CSchematycCoreCreator::~CSchematycCoreCreator() {}

const char* CSchematycCoreCreator::ms_szClassName = "GameExtension::SchematycCoreCreator";

ICore* CreateCore(IGameFramework* pIGameFramework)
{
	IGameFrameworkExtensionCreatorPtr pCreator;
	if (CryCreateClassInstance(CSchematycCoreCreator::ms_szClassName, pCreator))
	{
		return cryinterface_cast<ICore>(pCreator->Create(pIGameFramework, nullptr));
	}
	return nullptr;
}

CRYREGISTER_CLASS(CCore)

CCore::CCore()
	: m_pEnvRegistry(new CEnvRegistry())
	, m_pScriptRegistry(new CScriptRegistry())
	, m_pRuntimeRegistry(new CRuntimeRegistry())
	, m_pObjectPool(new CObjectPool())
	, m_pCompiler(new CCompiler())
	, m_pTimerSystem(new CTimerSystem())
	, m_pLog(new CLog())
	, m_pLogRecorder(new CLogRecorder())
	, m_pSettingsManager(new CSettingsManager())
	, m_pUpdateScheduler(new CUpdateScheduler())
{}

CCore::~CCore()
{
	m_pLog->Shutdown();
	Schematyc::CVars::Unregister();
}

void CCore::Init()
{
	gEnv->pSystem->GetIPluginManager()->LoadPluginFromDisk(ICryPluginManager::EPluginType::EPluginType_CPP, "CrySensorSystem", "Plugin_CrySensorSystem");

	Schematyc::CVars::Register();

	m_pLog->Init();
	if (CVars::sc_LogToFile)
	{
		CStackString logFileName;
		const int applicationInstance = gEnv->pSystem->GetApplicationInstance();
		if (applicationInstance)
		{
			logFileName.Format("schematyc(%d).log", applicationInstance);
		}
		else
		{
			logFileName = "schematyc.log";
		}
		m_pLogFileOutput = m_pLog->CreateFileOutput(logFileName.c_str());
		SCHEMATYC_CORE_ASSERT(m_pLogFileOutput);
		RefreshLogFileStreams();
		CVars::sc_LogFileStreams->SetOnChangeCallback(OnLogFileStreamsChange);
		RefreshLogFileMessageTypes();
		CVars::sc_LogFileMessageTypes->SetOnChangeCallback(OnLogFileMessageTypesChange);
	}

	if (CVars::sc_RunUnitTests)
	{
		CUnitTestRegistrar::RunUnitTests();
	}

	m_pEnvRegistry->RegisterPackage(SCHEMATYC_MAKE_ENV_PACKAGE("a67cd89b-a62c-417e-851c-85bc2ffafdc9"_schematyc_guid, "CoreEnv", Delegate::Make(&RegisterCoreEnvPackage)));
}

void CCore::RefreshEnv()
{
	// #SchematycTODO : Ensure that no objects exist?
	m_pRuntimeRegistry->Reset();
	m_pEnvRegistry->Refresh();
	// #SchematycTODO : Re-load and re-compile scripts?
}

void CCore::SetGUIDGenerator(const GUIDGenerator& guidGenerator)
{
	m_guidGenerator = guidGenerator;
}

SGUID CCore::CreateGUID() const
{
	return !m_guidGenerator.IsEmpty() ? m_guidGenerator() : SGUID();
}

const char* CCore::GetFileFormat() const
{
	return CVars::sc_FileFormat->GetString();
}

const char* CCore::GetRootFolder() const
{
	return CVars::sc_RootFolder->GetString();
}

const char* CCore::GetScriptsFolder() const
{
	m_scriptsFolder = GetRootFolder();
	m_scriptsFolder.append("/");
	m_scriptsFolder.append(g_szScriptsFolder);
	return m_scriptsFolder.c_str();
}

const char* CCore::GetSettingsFolder() const
{
	m_settingsFolder = GetRootFolder();
	m_settingsFolder.append("/");
	m_settingsFolder.append(g_szSettingsFolder);
	return m_settingsFolder.c_str();
}

bool CCore::IsExperimentalFeatureEnabled(const char* szFeatureName) const
{
	return CryStringUtils::stristr(CVars::sc_ExperimentalFeatures->GetString(), szFeatureName) != nullptr;
}

IEnvRegistry& CCore::GetEnvRegistry()
{
	SCHEMATYC_CORE_ASSERT(m_pEnvRegistry);
	return *m_pEnvRegistry;
}

IScriptRegistry& CCore::GetScriptRegistry()
{
	SCHEMATYC_CORE_ASSERT(m_pScriptRegistry);
	return *m_pScriptRegistry;
}

IRuntimeRegistry& CCore::GetRuntimeRegistry()
{
	SCHEMATYC_CORE_ASSERT(m_pRuntimeRegistry);
	return *m_pRuntimeRegistry;
}

ICompiler& CCore::GetCompiler()
{
	SCHEMATYC_CORE_ASSERT(m_pCompiler);
	return *m_pCompiler;
}

ILog& CCore::GetLog()
{
	return *m_pLog;
}

ILogRecorder& CCore::GetLogRecorder()
{
	return *m_pLogRecorder;
}

ISettingsManager& CCore::GetSettingsManager()
{
	return *m_pSettingsManager;
}

IUpdateScheduler& CCore::GetUpdateScheduler()
{
	return *m_pUpdateScheduler;
}

ITimerSystem& CCore::GetTimerSystem()
{
	return *m_pTimerSystem;
}

IValidatorArchivePtr CCore::CreateValidatorArchive(const SValidatorArchiveParams& params) const
{
	return std::make_shared<CValidatorArchive>(params);
}

ISerializationContextPtr CCore::CreateSerializationContext(const SSerializationContextParams& params) const
{
	return std::make_shared<CSerializationContext>(params);
}

IScriptViewPtr CCore::CreateScriptView(const SGUID& scopeGUID) const
{
	return std::make_shared<CScriptView>(scopeGUID);
}

IObject* CCore::CreateObject(const SObjectParams& params)
{
	return m_pObjectPool->CreateObject(params);
}

IObject* CCore::GetObject(ObjectId objectId)
{
	return m_pObjectPool->GetObject(objectId);
}

void CCore::DestroyObject(ObjectId objectId)
{
	m_pObjectPool->DestroyObject(objectId);
}

void CCore::SendSignal(ObjectId objectId, const SGUID& signalGUID, CRuntimeParams& params)
{
	m_pObjectPool->SendSignal(objectId, signalGUID, params);
}

void CCore::BroadcastSignal(const SGUID& signalGUID, CRuntimeParams& params)
{
	m_pObjectPool->BroadcastSignal(signalGUID, params);
}

void CCore::PrePhysicsUpdate()
{
	if (WantPrePhysicsUpdate())
	{
		m_pUpdateScheduler->BeginFrame(gEnv->pTimer->GetFrameTime());
		m_pUpdateScheduler->Update(EUpdateStage::PrePhysics | EUpdateDistribution::Earliest, EUpdateStage::PrePhysics | EUpdateDistribution::End);
	}
}

void CCore::Update()
{
	if (WantUpdate())
	{
		if (!m_pUpdateScheduler->InFrame())
		{
			m_pUpdateScheduler->BeginFrame(gEnv->pTimer->GetFrameTime());
		}

		if (gEnv->IsEditing())
		{
			m_pUpdateScheduler->Update(EUpdateStage::Editing | EUpdateDistribution::Earliest, EUpdateStage::Editing | EUpdateDistribution::End);
			m_pUpdateScheduler->EndFrame();
		}
		else
		{
			m_pTimerSystem->Update();

			m_pUpdateScheduler->Update(EUpdateStage::Default | EUpdateDistribution::Earliest, EUpdateStage::Post | EUpdateDistribution::End);
			m_pUpdateScheduler->EndFrame();
		}

		m_pLog->Update();
	}
}

void CCore::RefreshLogFileSettings()
{
	RefreshLogFileStreams();
	RefreshLogFileMessageTypes();
}

void CCore::RefreshLogFileStreams()
{
	if (m_pLogFileOutput)
	{
		m_pLogFileOutput->DisableAllStreams();
		CStackString logFileStreams = CVars::sc_LogFileStreams->GetString();
		const uint32 length = logFileStreams.length();
		int pos = 0;
		do
		{
			CStackString token = logFileStreams.Tokenize(" ", pos);
			const LogStreamId logStreamId = GetSchematycCore().GetLog().GetStreamId(token.c_str());
			if (logStreamId != LogStreamId::Invalid)
			{
				m_pLogFileOutput->EnableStream(logStreamId);
			}
		}
		while (pos < length);
	}
}

void CCore::RefreshLogFileMessageTypes()
{
	if (m_pLogFileOutput)
	{
		m_pLogFileOutput->DisableAllMessageTypes();
		CStackString logFileMessageTypes = CVars::sc_LogFileMessageTypes->GetString();
		const uint32 length = logFileMessageTypes.length();
		int pos = 0;
		do
		{
			CStackString token = logFileMessageTypes.Tokenize(" ", pos);
			const ELogMessageType logMessageType = GetSchematycCore().GetLog().GetMessageType(token.c_str());
			if (logMessageType != ELogMessageType::Invalid)
			{
				m_pLogFileOutput->EnableMessageType(logMessageType);
			}
		}
		while (pos < length);
	}
}

CRuntimeRegistry& CCore::GetRuntimeRegistryImpl()
{
	SCHEMATYC_CORE_ASSERT(m_pRuntimeRegistry);
	return *m_pRuntimeRegistry;
}

CCompiler& CCore::GetCompilerImpl()
{
	SCHEMATYC_CORE_ASSERT(m_pCompiler);
	return *m_pCompiler;
}

const char* CCore::ms_szClassName = "GameExtension::SchematycCore";
} // Schematyc