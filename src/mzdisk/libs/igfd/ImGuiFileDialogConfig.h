#pragma once

/**
 * Konfigurace ImGuiFileDialog pro mzdisk GUI.
 * Převzato z mz800new/src/ui-imgui/filechooser/headers/CustomImGuiFileDialogConfig.h
 * a upraveno (bez ContrastedButton a ToggleButton - používáme standardní ImGui widgety).
 */

#ifndef IMGUI_DEFINE_MATH_OPERATORS
#define IMGUI_DEFINE_MATH_OPERATORS
#endif

#include "CustomFont.h"
#include "i18n.h"
#include "libs/imwidgets/ImWidgets.h"

/////////////////////////////////
//// EXPLORATION BY KEYS ////////
/////////////////////////////////

#define USE_EXPLORATION_BY_KEYS
#define IGFD_KEY_UP ImGuiKey_UpArrow
#define IGFD_KEY_DOWN ImGuiKey_DownArrow
#define IGFD_KEY_ENTER ImGuiKey_Enter
#define IGFD_KEY_BACKSPACE ImGuiKey_Backspace

/////////////////////////////////
//// DIALOG EXIT ////////////////
/////////////////////////////////

#define USE_DIALOG_EXIT_WITH_KEY
#define IGFD_EXIT_KEY ImGuiKey_Escape

/////////////////////////////////
//// WIDGETS ////////////////////
/////////////////////////////////

#define FILTER_COMBO_AUTO_SIZE 1
#define FILTER_COMBO_MIN_WIDTH 50.0f

#define IMGUI_BEGIN_COMBO ImGui::BeginContrastedCombo
#define IMGUI_PATH_BUTTON ImGui::ContrastedButton_For_Dialogs
#define IMGUI_BUTTON ImGui::ContrastedButton_For_Dialogs

/////////////////////////////////
//// DISPLAY MODE ICONS /////////
/////////////////////////////////

#define DisplayMode_FilesList_ButtonString ICON_IGFD_FILE_LIST
#define DisplayMode_ThumbailsList_ButtonString ICON_IGFD_FILE_LIST_THUMBNAILS
#define DisplayMode_ThumbailsGrid_ButtonString ICON_IGFD_FILE_GRID_THUMBNAILS

/////////////////////////////////
//// ICON LABELS ////////////////
/////////////////////////////////

/* ikonové labely pro položky (adresáře, soubory, linky) */
#define dirEntryString ICON_IGFD_FOLDER
#define linkEntryString ICON_IGFD_LINK
#define fileEntryString ICON_IGFD_FILE

/* ikonové labely pro tlačítka */
#define createDirButtonString ICON_IGFD_ADD
#define resetButtonString ICON_IGFD_RESET
#define devicesButtonString ICON_IGFD_DRIVES
#define editPathButtonString ICON_IGFD_EDIT
#define searchString ICON_IGFD_SEARCH

/////////////////////////////////
//// TOOLTIPS + LABELS //////////
/////////////////////////////////

#define buttonResetSearchString N_("Reset search")
#define buttonDriveString N_("Devices")
#define buttonEditPathString N_("Edit path\nYou can also right click on path buttons")
#define buttonResetPathString N_("Reset to current directory")
#define buttonCreateDirString N_("Create Directory")

#define fileNameString N_("File Name:")
#define dirNameString N_("Directory Path:")

/////////////////////////////////
//// OVERWRITE DIALOG ///////////
/////////////////////////////////

#define OverWriteDialogTitleString N_("The selected file already exists!")
#define OverWriteDialogMessageString N_("Are you sure you want to overwrite it?")
#define OverWriteDialogConfirmButtonString N_("Confirm")
#define OverWriteDialogCancelButtonString N_("Cancel")

/////////////////////////////////
//// VALIDATION BUTTONS /////////
/////////////////////////////////

#define okButtonIcon ICON_IGFD_OK
#define okButtonString N_("OK")
#define cancelButtonIcon ICON_IGFD_CANCEL
#define cancelButtonString N_("Cancel")
#define okCancelButtonAlignement 0.0f

/////////////////////////////////
//// FILE SIZE UNITS ////////////
/////////////////////////////////

#define fileSizeBytes N_("B")
#define fileSizeKiloBytes N_("KB")
#define fileSizeMegaBytes N_("MB")
#define fileSizeGigaBytes N_("GB")

/////////////////////////////////
//// SORTING ICONS //////////////
/////////////////////////////////

#define USE_CUSTOM_SORTING_ICON
#define tableHeaderAscendingIcon ICON_IGFD_CHEVRON_UP
#define tableHeaderDescendingIcon ICON_IGFD_CHEVRON_DOWN
#define tableHeaderFileNameString N_("File name")
#define tableHeaderFileTypeString N_("Type")
#define tableHeaderFileSizeString N_("Size")
#define tableHeaderFileDateTimeString N_("Date")

/////////////////////////////////
//// PLACES FEATURES ////////////
/////////////////////////////////

#define USE_PLACES_FEATURE
#define PLACES_PANE_DEFAULT_SHOWN true
#define placesButtonString ICON_IGFD_PLACES
#define placesButtonHelpString N_("Places")
#define addPlaceButtonString ICON_IGFD_ADD
#define removePlaceButtonString ICON_IGFD_REMOVE
#define validatePlaceButtonString ICON_IGFD_OK
#define editPlaceButtonString ICON_IGFD_EDIT

//////////////////////////////////////
//// PLACES FEATURES : BOOKMARKS /////
//////////////////////////////////////

#define PLACES_BOOKMARK_DEFAULT_OPEPEND false
#define placesBookmarksGroupName ICON_IGFD_BOOKMARK " Bookmarks"
#define placesBookmarksDisplayOrder 0

//////////////////////////////////////
//// PLACES FEATURES : DEVICES ///////
//////////////////////////////////////

#define USE_PLACES_DEVICES
#define PLACES_DEVICES_DEFAULT_OPEPEND true
#define placesDevicesGroupName ICON_IGFD_DRIVES " Devices"
#define placesDevicesDisplayOrder 10
