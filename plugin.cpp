
#include <mutex>
#include "RE/C/ConsoleLog.h"
#include "logger.h"
#include "McmInjector.h"

void MessageListener(SKSE::MessagingInterface::Message* message) {
    switch (message->type) {
        // Descriptions are taken from the original skse64 library
        // See:
        // https://github.com/ianpatt/skse64/blob/09f520a2433747f33ae7d7c15b1164ca198932c3/skse64/PluginAPI.h#L193-L212
         
        case SKSE::MessagingInterface::kDataLoaded: { 
            auto* papyrusInterface = SKSE::GetPapyrusInterface();
            papyrusInterface->Register(MCM::BindPapyrusFunctions);
			RE::ConsoleLog::GetSingleton()->Print("MCM Injector Installed");
        }
    }
}

std::mutex serializeMutex;

void LoadCallback(SKSE::SerializationInterface* ssi) {
	std::lock_guard<std::mutex> lock(serializeMutex);
	
    //gfuncs::lastTimeGameWasLoaded = std::chrono::system_clock::now();

    logger::debug("LoadCallback started");

    std::uint32_t record, version, length;

    if (ssi) {
		while (ssi->GetNextRecordInfo(record, version, length)) {
			
		}
		
		logger::debug("LoadCallback complete");
    }
    else {
        logger::error("ssi doesn't exist, aborting load.");
    }
}

void SaveCallback(SKSE::SerializationInterface* ssi) {
	std::lock_guard<std::mutex> lock(serializeMutex);
    logger::debug("SaveCallback started");

    if (ssi) {
		
		// used only for testing ResolveFormID on created forms.
		// CreateForms::SaveForms(ssi);
		logger::trace("SaveCallback complete");
    }
    else {
        logger::error("ssi doesn't exist, aborting save.");
    }
}

// void FormDeleteCallback(RE::VMHandle handle){
// }

//init================================================================================================================================================================
SKSEPluginLoad(const SKSE::LoadInterface* skse) {
    SKSE::Init(skse);
	REL::Module::reset(); //make sure REL offsets are correct before touching them. Caused ctd when opening the game without this.
	
	//Load the address library now, while the working directory is still the game folder.
	//IDDB loads lazily on first use, and by then another mod may have changed the working directory.
	//To fix user reported error REL/IDDB.h(157): failed to open address library file
	(void)REL::IDDB::get();
	
    SetupLog("Data/SKSE/Plugins/McmInjector.ini");
	logger::info("REL base [{:X}]", REL::Module::get().base());
	
    SKSE::GetMessagingInterface()->RegisterListener(MessageListener);

    auto* serialization = SKSE::GetSerializationInterface();
    serialization->SetUniqueID('NPT0');
    serialization->SetSaveCallback(SaveCallback);
    serialization->SetLoadCallback(LoadCallback);
	// serialization->SetFormDeleteCallback(FormDeleteCallback);
    
    return true;
}