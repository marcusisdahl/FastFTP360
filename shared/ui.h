// SPDX-License-Identifier: MIT
#pragma once
namespace nb {
enum UiTab { TabNetwork, TabDisk, TabFtp, TabCount };
struct UiState {
    char ip[48],message[100],disk[100],ftp[100];
    // Current setting values as shown; main.cpp owns the choices.
    char buffers[32],cpu[24],diskTarget[16],diskBlock[16],drawing[16];
    // FTP login; an empty user means any user name and password.
    char ftpUser[24],ftpPassword[24];
    int phase,direction,duration,armed,completed,streams,rateLimit,options,downloadLimit,diskPhase,ftpPhase;
    int tab,focus,busy;
    double elapsed,rate,upload,download,mebibytes,diskTotal;
    UiState();
};
// Number of settings rows on a tab; D-pad up/down moves between them.
int SettingCount(int tab);
// Creates the Dear ImGui context, style and fonts. The renderer builds the
// font texture from ImGui::GetIO().Fonts afterwards.
void InitUi();
// Builds one frame. The caller sets io.DeltaTime first and draws
// ImGui::GetDrawData() afterwards.
void DrawUi(const UiState& state);
}
