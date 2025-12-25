#include "ui/error_box.hpp"
#include "ui/nvg_util.hpp"
#include "app.hpp"
#include "i18n.hpp"

namespace sphaira::ui {
namespace {

auto GetModule(Result rc) -> const char* {
    switch (R_MODULE(rc)) {
        case Module_Svc: return "Svc";
        case Module_Fs: return "Fs";
        case Module_Os: return "Os";
        case Module_Ncm: return "Ncm";
        case Module_Ns: return "Ns";
        case Module_Spl: return "Spl";
        case Module_Applet: return "Applet";
        case Module_Usb: return "Usb";
        case Module_Irsensor: return "Irsensor";
        case Module_Libnx: return "Libnx";
        case Module_Sphaira: return "HATS";
    }

    return nullptr;
}
auto GetCodeMessage(Result rc) -> const char* {
    switch (rc) {
        case SvcError_TimedOut: return "SvcError_TimedOut";
        case SvcError_Cancelled: return "SvcError_Cancelled";

        case FsError_PathNotFound: return "FsError_PathNotFound";
        case FsError_PathAlreadyExists: return "FsError_PathAlreadyExists";
        case FsError_TargetLocked: return "FsError_TargetLocked";
        case FsError_TooLongPath: return "FsError_TooLongPath";
        case FsError_InvalidCharacter: return "FsError_InvalidCharacter";
        case FsError_InvalidOffset: return "FsError_InvalidOffset";
        case FsError_InvalidSize: return "FsError_InvalidSize";

        case Result_TransferCancelled: return "HatsError_TransferCancelled";
        case Result_StreamBadSeek: return "HatsError_StreamBadSeek";
        case Result_FsTooManyEntries: return "HatsError_FsTooManyEntries";
        case Result_FsNewPathTooLarge: return "HatsError_FsNewPathTooLarge";
        case Result_FsInvalidType: return "HatsError_FsInvalidType";
        case Result_FsEmpty: return "HatsError_FsEmpty";
        case Result_FsAlreadyRoot: return "HatsError_FsAlreadyRoot";
        case Result_FsNoCurrentPath: return "HatsError_FsNoCurrentPath";
        case Result_FsBrokenCurrentPath: return "HatsError_FsBrokenCurrentPath";
        case Result_FsIndexOutOfBounds: return "HatsError_FsIndexOutOfBounds";
        case Result_FsFsNotActive: return "HatsError_FsFsNotActive";
        case Result_FsNewPathEmpty: return "HatsError_FsNewPathEmpty";
        case Result_FsLoadingCancelled: return "HatsError_FsLoadingCancelled";
        case Result_FsBrokenRoot: return "HatsError_FsBrokenRoot";
        case Result_FsUnknownStdioError: return "HatsError_FsUnknownStdioError";
        case Result_FsStdioFailedToSeek: return "HatsError_FsStdioFailedToSeek";
        case Result_FsStdioFailedToRead: return "HatsError_FsStdioFailedToRead";
        case Result_FsStdioFailedToWrite: return "HatsError_FsStdioFailedToWrite";
        case Result_FsStdioFailedToOpenFile: return "HatsError_FsStdioFailedToOpenFile";
        case Result_FsStdioFailedToCreate: return "HatsError_FsStdioFailedToCreate";
        case Result_FsStdioFailedToTruncate: return "HatsError_FsStdioFailedToTruncate";
        case Result_FsStdioFailedToFlush: return "HatsError_FsStdioFailedToFlush";
        case Result_FsStdioFailedToCreateDirectory: return "HatsError_FsStdioFailedToCreateDirectory";
        case Result_FsStdioFailedToDeleteFile: return "HatsError_FsStdioFailedToDeleteFile";
        case Result_FsStdioFailedToDeleteDirectory: return "HatsError_FsStdioFailedToDeleteDirectory";
        case Result_FsStdioFailedToOpenDirectory: return "HatsError_FsStdioFailedToOpenDirectory";
        case Result_FsStdioFailedToRename: return "HatsError_FsStdioFailedToRename";
        case Result_FsStdioFailedToStat: return "HatsError_FsStdioFailedToStat";
        case Result_FsReadOnly: return "HatsError_FsReadOnly";
        case Result_FsNotActive: return "HatsError_FsNotActive";
        case Result_FsFailedStdioStat: return "HatsError_FsFailedStdioStat";
        case Result_FsFailedStdioOpendir: return "HatsError_FsFailedStdioOpendir";
        case Result_NroBadMagic: return "HatsError_NroBadMagic";
        case Result_NroBadSize: return "HatsError_NroBadSize";
        case Result_AppFailedMusicDownload: return "HatsError_AppFailedMusicDownload";
        case Result_CurlFailedEasyInit: return "HatsError_CurlFailedEasyInit";
        case Result_DumpFailedNetworkUpload: return "HatsError_DumpFailedNetworkUpload";
        case Result_UnzOpen2_64: return "HatsError_UnzOpen2_64";
        case Result_UnzGetGlobalInfo64: return "HatsError_UnzGetGlobalInfo64";
        case Result_UnzLocateFile: return "HatsError_UnzLocateFile";
        case Result_UnzGoToFirstFile: return "HatsError_UnzGoToFirstFile";
        case Result_UnzGoToNextFile: return "HatsError_UnzGoToNextFile";
        case Result_UnzOpenCurrentFile: return "HatsError_UnzOpenCurrentFile";
        case Result_UnzGetCurrentFileInfo64: return "HatsError_UnzGetCurrentFileInfo64";
        case Result_UnzReadCurrentFile: return "HatsError_UnzReadCurrentFile";
        case Result_ZipOpen2_64: return "HatsError_ZipOpen2_64";
        case Result_ZipOpenNewFileInZip: return "HatsError_ZipOpenNewFileInZip";
        case Result_ZipWriteInFileInZip: return "HatsError_ZipWriteInFileInZip";
        case Result_MmzBadLocalHeaderSig: return "HatsError_MmzBadLocalHeaderSig";
        case Result_MmzBadLocalHeaderRead: return "HatsError_MmzBadLocalHeaderRead";
        case Result_FileBrowserFailedUpload: return "HatsError_FileBrowserFailedUpload";
        case Result_FileBrowserDirNotDaybreak: return "HatsError_FileBrowserDirNotDaybreak";
        case Result_AppstoreFailedZipDownload: return "HatsError_AppstoreFailedZipDownload";
        case Result_AppstoreFailedMd5: return "HatsError_AppstoreFailedMd5";
        case Result_AppstoreFailedParseManifest: return "HatsError_AppstoreFailedParseManifest";
        case Result_GameBadReadForDump: return "HatsError_GameBadReadForDump";
        case Result_GameEmptyMetaEntries: return "HatsError_GameEmptyMetaEntries";
        case Result_GameMultipleKeysFound: return "HatsError_GameMultipleKeysFound";
        case Result_GameNoNspEntriesBuilt: return "HatsError_GameNoNspEntriesBuilt";
        case Result_KeyMissingNcaKeyArea: return "HatsError_KeyMissingNcaKeyArea";
        case Result_KeyMissingTitleKek: return "HatsError_KeyMissingTitleKek";
        case Result_KeyMissingMasterKey: return "HatsError_KeyMissingMasterKey";
        case Result_KeyFailedDecyptETicketDeviceKey: return "HatsError_KeyFailedDecyptETicketDeviceKey";
        case Result_NcaFailedNcaHeaderHashVerify: return "HatsError_NcaFailedNcaHeaderHashVerify";
        case Result_NcaBadSigKeyGen: return "HatsError_NcaBadSigKeyGen";
        case Result_GcBadReadForDump: return "HatsError_GcBadReadForDump";
        case Result_GcEmptyGamecard: return "HatsError_GcEmptyGamecard";
        case Result_GcBadXciMagic: return "HatsError_GcBadXciMagic";
        case Result_GcBadXciRomSize: return "HatsError_GcBadXciRomSize";
        case Result_GcFailedToGetSecurityInfo: return "HatsError_GcFailedToGetSecurityInfo";
        case Result_GhdlEmptyAsset: return "HatsError_GhdlEmptyAsset";
        case Result_GhdlFailedToDownloadAsset: return "HatsError_GhdlFailedToDownloadAsset";
        case Result_GhdlFailedToDownloadAssetJson: return "HatsError_GhdlFailedToDownloadAssetJson";
        case Result_ThemezerFailedToDownloadThemeMeta: return "HatsError_ThemezerFailedToDownloadThemeMeta";
        case Result_ThemezerFailedToDownloadTheme: return "HatsError_ThemezerFailedToDownloadTheme";
        case Result_MainFailedToDownloadUpdate: return "HatsError_MainFailedToDownloadUpdate";
        case Result_UsbDsBadDeviceSpeed: return "HatsError_UsbDsBadDeviceSpeed";
        case Result_NcaBadMagic: return "HatsError_NcaBadMagic";
        case Result_NspBadMagic: return "HatsError_NspBadMagic";
        case Result_XciBadMagic: return "HatsError_XciBadMagic";
        case Result_XciSecurePartitionNotFound: return "HatsError_XciSecurePartitionNotFound";
        case Result_EsBadTitleKeyType: return "HatsError_EsBadTitleKeyType";
        case Result_EsPersonalisedTicketDeviceIdMissmatch: return "HatsError_EsPersonalisedTicketDeviceIdMissmatch";
        case Result_EsFailedDecryptPersonalisedTicket: return "HatsError_EsFailedDecryptPersonalisedTicket";
        case Result_EsBadDecryptedPersonalisedTicketSize: return "HatsError_EsBadDecryptedPersonalisedTicketSize";
        case Result_EsInvalidTicketBadRightsId: return "HatsError_EsInvalidTicketBadRightsId";
        case Result_EsInvalidTicketFromatVersion: return "HatsError_EsInvalidTicketFromatVersion";
        case Result_EsInvalidTicketKeyType: return "HatsError_EsInvalidTicketKeyType";
        case Result_EsInvalidTicketKeyRevision: return "HatsError_EsInvalidTicketKeyRevision";
        case Result_OwoBadArgs: return "HatsError_OwoBadArgs";
        case Result_UsbCancelled: return "HatsError_UsbCancelled";
        case Result_UsbBadMagic: return "HatsError_UsbBadMagic";
        case Result_UsbBadVersion: return "HatsError_UsbBadVersion";
        case Result_UsbBadCount: return "HatsError_UsbBadCount";
        case Result_UsbBadBufferAlign: return "HatsError_UsbBadBufferAlign";
        case Result_UsbBadTransferSize: return "HatsError_UsbBadTransferSize";
        case Result_UsbEmptyTransferSize: return "HatsError_UsbEmptyTransferSize";
        case Result_UsbOverflowTransferSize: return "HatsError_UsbOverflowTransferSize";
        case Result_UsbUploadBadMagic: return "HatsError_UsbUploadBadMagic";
        case Result_UsbUploadExit: return "HatsError_UsbUploadExit";
        case Result_UsbUploadBadCount: return "HatsError_UsbUploadBadCount";
        case Result_UsbUploadBadTransferSize: return "HatsError_UsbUploadBadTransferSize";
        case Result_UsbUploadBadTotalSize: return "HatsError_UsbUploadBadTotalSize";
        case Result_UsbUploadBadCommand: return "HatsError_UsbUploadBadCommand";
        case Result_YatiContainerNotFound: return "HatsError_YatiContainerNotFound";
        case Result_YatiNcaNotFound: return "HatsError_YatiNcaNotFound";
        case Result_YatiInvalidNcaReadSize: return "HatsError_YatiInvalidNcaReadSize";
        case Result_YatiInvalidNcaSigKeyGen: return "HatsError_YatiInvalidNcaSigKeyGen";
        case Result_YatiInvalidNcaMagic: return "HatsError_YatiInvalidNcaMagic";
        case Result_YatiInvalidNcaSignature0: return "HatsError_YatiInvalidNcaSignature0";
        case Result_YatiInvalidNcaSignature1: return "HatsError_YatiInvalidNcaSignature1";
        case Result_YatiInvalidNcaSha256: return "HatsError_YatiInvalidNcaSha256";
        case Result_YatiNczSectionNotFound: return "HatsError_YatiNczSectionNotFound";
        case Result_YatiInvalidNczSectionCount: return "HatsError_YatiInvalidNczSectionCount";
        case Result_YatiNczBlockNotFound: return "HatsError_YatiNczBlockNotFound";
        case Result_YatiInvalidNczBlockVersion: return "HatsError_YatiInvalidNczBlockVersion";
        case Result_YatiInvalidNczBlockType: return "HatsError_YatiInvalidNczBlockType";
        case Result_YatiInvalidNczBlockTotal: return "HatsError_YatiInvalidNczBlockTotal";
        case Result_YatiInvalidNczBlockSizeExponent: return "HatsError_YatiInvalidNczBlockSizeExponent";
        case Result_YatiInvalidNczZstdError: return "HatsError_YatiInvalidNczZstdError";
        case Result_YatiTicketNotFound: return "HatsError_YatiTicketNotFound";
        case Result_YatiInvalidTicketBadRightsId: return "HatsError_YatiInvalidTicketBadRightsId";
        case Result_YatiCertNotFound: return "HatsError_YatiCertNotFound";
        case Result_YatiNcmDbCorruptHeader: return "HatsError_YatiNcmDbCorruptHeader";
        case Result_YatiNcmDbCorruptInfos: return "HatsError_YatiNcmDbCorruptInfos";

        case Result_NszFailedCreateCctx: return "HatsError_NszFailedCreateCctx";
        case Result_NszFailedSetCompressionLevel: return "HatsError_NszFailedSetCompressionLevel";
        case Result_NszFailedSetThreadCount: return "HatsError_NszFailedSetThreadCount";
        case Result_NszFailedSetLongDistanceMode: return "HatsError_NszFailedSetLongDistanceMode";
        case Result_NszFailedResetCctx: return "HatsError_NszFailedResetCctx";
        case Result_NszFailedCompress2: return "HatsError_NszFailedCompress2";
        case Result_NszFailedCompressStream2: return "HatsError_NszFailedCompressStream2";
        case Result_NszTooManyBlocks: return "HatsError_NszTooManyBlocks";
        case Result_NszMissingBlocks: return "HatsError_NszMissingBlocks";
    }

    return "";
}

} // namespace

ErrorBox::ErrorBox(const std::string& message) : m_message{message} {
    log_write("[ERROR] %s\n", m_message.c_str());

    m_pos.w = 770.f;
    m_pos.h = 430.f;
    m_pos.x = 255;
    m_pos.y = 145;

    SetAction(Button::A, Action{[this](){
        SetPop();
    }});

    App::PlaySoundEffect(SoundEffect::Error);
}

ErrorBox::ErrorBox(Result code, const std::string& message) : ErrorBox{message} {
    m_code = code;
    m_code_message = GetCodeMessage(code);
    m_code_module = std::to_string(R_MODULE(code));
    if (auto str = GetModule(code)) {
        m_code_module += " (" + std::string(str) + ")";
    }
    log_write("[ERROR] Code: 0x%X Module: %s Description: %u\n", R_VALUE(code), m_code_module.c_str(), R_DESCRIPTION(code));
}

auto ErrorBox::Update(Controller* controller, TouchInfo* touch) -> void {
    Widget::Update(controller, touch);
}

auto ErrorBox::Draw(NVGcontext* vg, Theme* theme) -> void {
    gfx::dimBackground(vg);
    gfx::drawRect(vg, m_pos, theme->GetColour(ThemeEntryID_POPUP));

    const Vec4 box = { 455, 470, 365, 65 };
    const auto center_x = m_pos.x + m_pos.w/2;

    gfx::drawTextArgs(vg, center_x, 180, 63, NVG_ALIGN_CENTER | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_ERROR), "\uE140");
    if (m_code.has_value()) {
        const auto code = m_code.value();
        if (m_code_message.empty()) {
            gfx::drawTextArgs(vg, center_x, 270, 25, NVG_ALIGN_CENTER | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT), "Code: 0x%X Module: %s", R_VALUE(code), m_code_module.c_str());
        } else {
            gfx::drawTextArgs(vg, center_x, 270, 25, NVG_ALIGN_CENTER | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT), "%s", m_code_message.c_str());
        }
    } else {
        gfx::drawTextArgs(vg, center_x, 270, 25, NVG_ALIGN_CENTER | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT), "An error occurred"_i18n.c_str());
    }
    gfx::drawTextArgs(vg, center_x, 325, 23, NVG_ALIGN_CENTER | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT), "%s", m_message.c_str());
    gfx::drawTextArgs(vg, center_x, 380, 20, NVG_ALIGN_CENTER | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT_INFO), "If this message appears repeatedly, please open an issue."_i18n.c_str());
    gfx::drawTextArgs(vg, center_x, 415, 20, NVG_ALIGN_CENTER | NVG_ALIGN_TOP, theme->GetColour(ThemeEntryID_TEXT_INFO), "https://github.com/sthetix/HATS-Tool/issues");
    gfx::drawRectOutline(vg, theme, 4.f, box);
    gfx::drawTextArgs(vg, center_x, box.y + box.h/2, 23, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, theme->GetColour(ThemeEntryID_TEXT_SELECTED), "OK"_i18n.c_str());
}

} // namespace sphaira::ui
