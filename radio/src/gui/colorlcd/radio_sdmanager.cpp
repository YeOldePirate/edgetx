/*
 * Copyright (C) EdgeTX
 *
 * Based on code named
 *   opentx - https://github.com/opentx/opentx
 *   th9x - http://code.google.com/p/th9x
 *   er9x - http://code.google.com/p/er9x
 *   gruvin9x - http://code.google.com/p/gruvin9x
 *
 * License GPLv2: http://www.gnu.org/licenses/gpl-2.0.html
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "radio_sdmanager.h"
#include "opentx.h"
#include "libopenui.h"
#include "io/frsky_firmware_update.h"
#include "io/multi_firmware_update.h"
#include "io/bootloader_flash.h"
#include "standalone_lua.h"
#include "sdcard.h"
#include "view_text.h"
#include "file_preview.h"

constexpr int WARN_FILE_LENGTH = 96000;

class FileNameEditWindow : public Page
{
  public:
  FileNameEditWindow(const std::string iName) :
      Page(ICON_RADIO_SD_MANAGER), name(std::move(iName))
  {
    buildHeader(&header);
    buildBody(&body);
  };

#if defined(DEBUG_WINDOWS)
  std::string getName() const override { return "FileNameEditWindow"; }
#endif
  protected:
  const std::string name;

  void buildHeader(Window *window)
  {
    new StaticText(window, {PAGE_TITLE_LEFT, PAGE_TITLE_TOP + 10, LCD_W - PAGE_TITLE_LEFT, PAGE_LINE_HEIGHT}, STR_RENAME_FILE, 0, COLOR_THEME_PRIMARY2);
  }

  void buildBody(Window *window)
  {
    GridLayout grid(window);
    grid.spacer(8);
    uint8_t nameLength;
    uint8_t extLength;
    char extension[LEN_FILE_EXTENSION_MAX + 1];
    memset(extension, 0, sizeof(extension));
    const char *ext =
        getFileExtension(name.c_str(), 0, 0, &nameLength, &extLength);

    if (extLength > LEN_FILE_EXTENSION_MAX) extLength = LEN_FILE_EXTENSION_MAX;
    if (ext) strncpy(extension, ext, extLength);

    const uint8_t maxNameLength = SD_SCREEN_FILE_LENGTH - extLength;
    nameLength -= extLength;
    if (nameLength > maxNameLength) nameLength = maxNameLength;
    memset(reusableBuffer.sdManager.originalName, 0, SD_SCREEN_FILE_LENGTH);

    strncpy(reusableBuffer.sdManager.originalName, name.c_str(), nameLength);
    reusableBuffer.sdManager.originalName[nameLength] = '\0';

    auto newFileName = new TextEdit(
        window, grid.getSlot(), reusableBuffer.sdManager.originalName,
        SD_SCREEN_FILE_LENGTH - extLength, LcdFlags(0));
    newFileName->setChangeHandler([=]() {
      char *newValue = reusableBuffer.sdManager.originalName;
      size_t totalSize = strlen(newValue);
      char changedName[SD_SCREEN_FILE_LENGTH + 1];
      memset(changedName, 0, sizeof(changedName));
      strncpy(changedName, newValue, totalSize);
      changedName[totalSize] = '\0';
      if (extLength) {
        strncpy(changedName + totalSize, extension, extLength);
      }
      changedName[totalSize + extLength] = '\0';
      f_rename((const TCHAR *)name.c_str(), (const TCHAR *)changedName);
    });
  };
};

RadioSdManagerPage::RadioSdManagerPage() :
  PageTab(SD_IS_HC() ? STR_SDHC_CARD : STR_SD_CARD, ICON_RADIO_SD_MANAGER)
{
  setOnSetVisibleHandler([]() {
    TRACE("f_chdir(ROOT_PATH)");
    f_chdir(ROOT_PATH);
  });
}

void RadioSdManagerPage::rebuild(FormWindow * window)
{
  coord_t scrollPosition = window->getScrollPositionY();
  window->clear();
  build(window);
  window->setScrollPositionY(scrollPosition);
}

// TODO elsewhere
extern bool compare_nocase(const std::string &first, const std::string &second);


char * getFullPath(const std::string &filename)
{
  static char full_path[FF_MAX_LFN + 1]; // TODO optimize that!
  f_getcwd((TCHAR*)full_path, FF_MAX_LFN);
  strcat(full_path, "/");
  strcat(full_path, filename.c_str());
  return full_path;
}

char * getCurrentPath()
{
  static char path[FF_MAX_LFN + 1]; // TODO optimize that!
  f_getcwd((TCHAR*)path, FF_MAX_LFN);
  return path;
}


template <class T>
class FlashDialog: public FullScreenDialog
{
  public:
    explicit FlashDialog(const T & device):
      FullScreenDialog(WARNING_TYPE_INFO, STR_FLASH_DEVICE),
      device(device),
      progress(this, {LCD_W / 2 - 50, LCD_H / 2, 100, 15})
    {
      setFocus();
    }

    void deleteLater(bool detach = true, bool trash = true) override
    {
      if (_deleted)
        return;

      progress.deleteLater(true, false);
      FullScreenDialog::deleteLater(detach, trash);
    }

    void flash(const char * filename)
    {
      TRACE("flashing '%s'", filename);
      device.flashFirmware(
          filename,
          [=](const char *title, const char *message, int count,
              int total) -> void {
            setMessage(message);
            progress.setValue(total > 0 ? count * 100 / total : 0);
            MainWindow::instance()->run(false);
          });
      deleteLater();
    }

  protected:
    T device;
    Progress progress;
};

class SDmanagerButton : public TextButton
{
  public:
    SDmanagerButton(FormGroup* parent, const rect_t& rect, std::string text,
              std::function<uint8_t(void)> pressHandler = nullptr,
              WindowFlags windowFlags = BUTTON_BACKGROUND | OPAQUE,
              LcdFlags textFlags = 0 ) : TextButton(parent, rect, text, pressHandler, windowFlags, textFlags)
              {
              }; 
#if defined(HARDWARE_TOUCH)
  bool onTouchStart(coord_t x, coord_t y) override
  {
    if (enabled) {
      if (!(windowFlags & NO_FOCUS)) {
        setFocus(SET_FOCUS_DEFAULT);
      }
    }
    return true;
  }
#endif              
};

void RadioSdManagerPage::build(FormWindow * window)
{
  FormGridLayout grid;
  grid.spacer(PAGE_PADDING);

  FILINFO fno;
  DIR dir;
  std::list<std::string> files;
  std::list<std::string> directories;

  std::string currentPath(getCurrentPath());
  auto preview = new FilePreview(window, {LCD_W / 2 + 6, 0, LCD_W / 2 - 16, window->height()});

  FRESULT res = f_opendir(&dir, "."); // Open the directory
  if (res == FR_OK) {
    // read all entries
    bool firstTime = true;
    for (;;) {
      res = sdReadDir(&dir, &fno, firstTime);

      if (res != FR_OK || fno.fname[0] == 0)
        break; // Break on error or end of dir
      if (strlen((const char*)fno.fname) > SD_SCREEN_FILE_LENGTH)
        continue;
      if (fno.fname[0] == '.' && fno.fname[1] != '.')
        continue; // Ignore hidden files under UNIX, but not ..

      if (fno.fattrib & AM_DIR) {
        directories.push_back((char*)fno.fname);
      } else {
        files.push_back((char*)fno.fname);
      }
    }

    // sort directories and files
    directories.sort(compare_nocase);
    files.sort(compare_nocase);

    for (auto name: directories) {
      new SDmanagerButton(window, grid.getLabelSlot(), name, [=]() -> uint8_t {
          std::string fullpath = currentPath + "/" + name;
          f_chdir((TCHAR*)fullpath.c_str());
          window->clear();
          build(window);
          return 0;
      });

      grid.nextLine();
    }

    for (auto name: files) {
      auto button = new SDmanagerButton(window, grid.getLabelSlot(), name, [=]() -> uint8_t {
          auto menu = new Menu(window);
          f_chdir(currentPath.c_str());
          const char *ext = getFileExtension(name.c_str());
          if (ext) {
            if (!strcasecmp(ext, SOUNDS_EXT)) {
              menu->addLine(STR_PLAY_FILE, [=]() {
                  audioQueue.stopAll();
                  audioQueue.playFile(getFullPath(name), 0, ID_PLAY_FROM_SD_MANAGER);
              });
            }
#if defined(MULTIMODULE) && !defined(DISABLE_MULTI_UPDATE)
            if (!READ_ONLY() && !strcasecmp(ext, MULTI_FIRMWARE_EXT)) {
              MultiFirmwareInformation information;
              if (information.readMultiFirmwareInformation(name.c_str()) == nullptr) {
#if defined(INTERNAL_MODULE_MULTI)
                menu->addLine(STR_FLASH_INTERNAL_MULTI, [=]() {
                  MultiFirmwareUpdate(name, INTERNAL_MODULE,
                                      MULTI_TYPE_MULTIMODULE);
                });
#endif
                menu->addLine(STR_FLASH_EXTERNAL_MULTI, [=]() {
                  MultiFirmwareUpdate(name, EXTERNAL_MODULE,
                                      MULTI_TYPE_MULTIMODULE);
                });
              }
            }
#endif
            else if (!READ_ONLY() && !strcasecmp(ext, ELRS_FIRMWARE_EXT)) {
              menu->addLine(STR_FLASH_EXTERNAL_ELRS, [=]() {
                MultiFirmwareUpdate(name, EXTERNAL_MODULE, MULTI_TYPE_ELRS);
              });
            }
            else if (isExtensionMatching(ext, BITMAPS_EXT)) {
               menu->addLine(STR_ASSIGN_BITMAP, [=]() {
                 memcpy(g_model.header.bitmap, name.c_str(),
                        sizeof(g_model.header.bitmap));
                 storageDirty(EE_MODEL);
               });
            }
            else if (!strcasecmp(ext, TEXT_EXT) || !strcasecmp(ext, LOGS_EXT)) {
              menu->addLine(STR_VIEW_TEXT, [=]() {
                static char lfn[FF_MAX_LFN + 1];  // TODO optimize that!
                f_getcwd((TCHAR *)lfn, FF_MAX_LFN);
                FIL file;
                std::string fileName =
                    std::string(lfn) + "/" + std::string(name);
                if (FR_OK == f_open(&file, (TCHAR *)fileName.c_str(),
                                    FA_OPEN_EXISTING | FA_READ)) {
                  const int fileLenght = file.obj.objsize;
                  f_close(&file);

                  if (fileLenght > WARN_FILE_LENGTH) {
                    char buf[64];
                    sprintf(buf, " %s %dkB. %s", STR_FILE_SIZE,
                            fileLenght / 1024, STR_FILE_OPEN);
                    new ConfirmDialog(window, STR_WARNING, buf, [=] {
                      auto textView = new ViewTextWindow(lfn, name);
                      textView->setCloseHandler([=]() { rebuild(window); });
                    });
                  } else {
                    auto textView = new ViewTextWindow(lfn, name);
                    textView->setCloseHandler([=]() { rebuild(window); });
                  }
                }
              });
            }
            if (!READ_ONLY() && !strcasecmp(ext, FIRMWARE_EXT)) {
              if (isBootloader(name.c_str())) {
                menu->addLine(STR_FLASH_BOOTLOADER, [=]() {
                  BootloaderUpdate(name);
                });
              }
            } else if (!READ_ONLY() && !strcasecmp(ext, SPORT_FIRMWARE_EXT)) {
              if (HAS_SPORT_UPDATE_CONNECTOR()) {
                menu->addLine(STR_FLASH_EXTERNAL_DEVICE, [=]() {
                  FrSkyFirmwareUpdate(name, SPORT_MODULE);
                });
              }
              menu->addLine(STR_FLASH_INTERNAL_MODULE, [=]() {
                FrSkyFirmwareUpdate(name, INTERNAL_MODULE);
              });
              menu->addLine(STR_FLASH_EXTERNAL_MODULE, [=]() {
                FrSkyFirmwareUpdate(name, EXTERNAL_MODULE);
              });
            } else if (!READ_ONLY() && !strcasecmp(ext, FRSKY_FIRMWARE_EXT)) {
              FrSkyFirmwareInformation information;
              if (readFrSkyFirmwareInformation(getFullPath(name),
                                               information) == nullptr) {
#if defined(INTERNAL_MODULE_PXX1) || defined(INTERNAL_MODULE_PXX2)
                menu->addLine(STR_FLASH_INTERNAL_MODULE, [=]() {
                  FrSkyFirmwareUpdate(name, INTERNAL_MODULE);
                });
#endif
                if (information.productFamily ==
                    FIRMWARE_FAMILY_EXTERNAL_MODULE) {
                  menu->addLine(STR_FLASH_EXTERNAL_MODULE, [=]() {
                    FrSkyFirmwareUpdate(name, EXTERNAL_MODULE);
                  });
                }
                if (information.productFamily == FIRMWARE_FAMILY_RECEIVER ||
                    information.productFamily == FIRMWARE_FAMILY_SENSOR) {
                  if (HAS_SPORT_UPDATE_CONNECTOR()) {
                    menu->addLine(STR_FLASH_EXTERNAL_DEVICE, [=]() {
                      FrSkyFirmwareUpdate(name, SPORT_MODULE);
                    });
                  } else {
                    menu->addLine(STR_FLASH_EXTERNAL_MODULE, [=]() {
                      FrSkyFirmwareUpdate(name, EXTERNAL_MODULE);
                    });
                  }
                }
// TODO: Integrate the remaining options
#if 0
#if defined(PXX2)
                if (information.productFamily == FIRMWARE_FAMILY_RECEIVER) {
                  if (isReceiverOTAEnabledFromModule(INTERNAL_MODULE,
                                                     information.productId))
                    menu->addLine(
                        STR_FLASH_RECEIVER_BY_INTERNAL_MODULE_OTA, [=]() {
                          FrSkyFirmwareUpdate(name, INTERNAL_MODULE_OTA);
                        });
                  if (isReceiverOTAEnabledFromModule(EXTERNAL_MODULE,
                                                     information.productId))
                    menu->addLine(
                        STR_FLASH_RECEIVER_BY_EXTERNAL_MODULE_OTA, [=]() {
                          FrSkyFirmwareUpdate(name, EXTERNAL_MODULE_OTA);
                        });
                }
                if (information.productFamily ==
                    FIRMWARE_FAMILY_FLIGHT_CONTROLLER) {
                  menu->addLine(
                      STR_FLASH_FLIGHT_CONTROLLER_BY_INTERNAL_MODULE_OTA,
                      [=]() {
                        FrSkyFirmwareUpdate(
                            name,
                            STR_FLASH_FLIGHT_CONTROLLER_BY_INTERNAL_MODULE_OTA);
                      });
                  menu->addLine(
                      STR_FLASH_FLIGHT_CONTROLLER_BY_EXTERNAL_MODULE_OTA,
                      [=]() {
                        FrSkyFirmwareUpdate(
                            name,
                            STR_FLASH_FLIGHT_CONTROLLER_BY_INTERNAL_MODULE_OTA);
                      });
                }
#endif
#if defined(BLUETOOTH)
                if (information.productFamily ==
                    FIRMWARE_FAMILY_BLUETOOTH_CHIP) {
                  menu->addLine(STR_FLASH_BLUETOOTH_MODULE, [=]() {
                    FrSkyFirmwareUpdate(name, STR_FLASH_BLUETOOTH_MODULE);
                  });
                }
#endif
#if defined(HARDWARE_POWER_MANAGEMENT_UNIT)
                if (information.productFamily ==
                    FIRMWARE_FAMILY_POWER_MANAGEMENT_UNIT) {
                  menu->addLine(STR_FLASH_POWER_MANAGEMENT_UNIT, [=]() {
                    FrSkyFirmwareUpdate(name, STR_FLASH_POWER_MANAGEMENT_UNIT);
                  });
                }
#endif
#endif
              }
            }
#if defined(LUA)
            else if (isExtensionMatching(ext, SCRIPTS_EXT)) {
              std::string fullpath = currentPath + "/" + name;
              menu->addLine(STR_EXECUTE_FILE, [=]() {
                luaExec(fullpath.c_str());
                StandaloneLuaWindow::instance()->attach(window);
              });
            }
#endif
          }
          if (!READ_ONLY()) {
            menu->addLine(STR_COPY_FILE, [=]() {
              clipboard.type = CLIPBOARD_TYPE_SD_FILE;
              f_getcwd(clipboard.data.sd.directory, CLIPBOARD_PATH_LEN);
              strncpy(clipboard.data.sd.filename, name.c_str(),
                      CLIPBOARD_PATH_LEN - 1);
            });
            if (clipboard.type == CLIPBOARD_TYPE_SD_FILE) {
              menu->addLine(STR_PASTE, [=]() {
                static char lfn[FF_MAX_LFN + 1];  // TODO optimize that!
                f_getcwd((TCHAR *)lfn, FF_MAX_LFN);
                // prevent copying to the same directory with the same name
                char *destNamePtr = clipboard.data.sd.filename;
                if (!strcmp(clipboard.data.sd.directory, lfn)) {
                  char destFileName[2 * CLIPBOARD_PATH_LEN + 1];
                  destNamePtr = strAppend(destFileName, FILE_COPY_PREFIX,
                                       CLIPBOARD_PATH_LEN);
                  destNamePtr = strAppend(destNamePtr, clipboard.data.sd.filename,
                                       CLIPBOARD_PATH_LEN);
                  destNamePtr = destFileName;
                }
                sdCopyFile(clipboard.data.sd.filename,
                           clipboard.data.sd.directory, destNamePtr, lfn);
                clipboard.type = CLIPBOARD_TYPE_NONE;

                rebuild(window);
              });
            }
            menu->addLine(STR_RENAME_FILE, [=]() {
              auto few = new FileNameEditWindow(name);
              few->setCloseHandler([=]() {
                //window->clear();
                rebuild(window);
              });
            });
            menu->addLine(STR_DELETE_FILE, [=]() {
                f_unlink((const TCHAR*)getFullPath(name));
                // coord_t scrollPosition = window->getScrollPositionY();
                window->clear();
                build(window);
                // window->setScrollPositionY(scrollPosition);
            });
          }
          return 0;
      }, BUTTON_BACKGROUND, COLOR_THEME_PRIMARY1);
      button->setFocusHandler([=](bool active) {
        if (active) {
          preview->setFile(getFullPath(name));
        }
      });
      grid.nextLine();
    }
  }

  window->setInnerHeight(grid.getWindowHeight());
  preview->setHeight(max(window->height(), grid.getWindowHeight()));
}

void RadioSdManagerPage::BootloaderUpdate(const std::string name)
{
  BootloaderFirmwareUpdate bootloaderFirmwareUpdate;
  auto dialog =
      new FlashDialog<BootloaderFirmwareUpdate>(bootloaderFirmwareUpdate);
  dialog->flash(getFullPath(name));
}

void RadioSdManagerPage::FrSkyFirmwareUpdate(const std::string name,
                                             ModuleIndex module)
{
  FrskyDeviceFirmwareUpdate deviceFirmwareUpdate(module);
  auto dialog =
      new FlashDialog<FrskyDeviceFirmwareUpdate>(deviceFirmwareUpdate);
  dialog->flash(getFullPath(name));
}

void RadioSdManagerPage::MultiFirmwareUpdate(const std::string name,
                                             ModuleIndex module,
                                             MultiModuleType type)
{
  MultiDeviceFirmwareUpdate deviceFirmwareUpdate(module, type);
  auto dialog =
      new FlashDialog<MultiDeviceFirmwareUpdate>(deviceFirmwareUpdate);
  dialog->flash(getFullPath(name));
}

#if 0
bool menuRadioSdManagerInfo(event_t event)
{
  SIMPLE_SUBMENU(STR_SD_INFO_TITLE, ICON_RADIO_SD_MANAGER, 1);

  lcdDrawText(MENUS_MARGIN_LEFT, 2*FH, STR_SD_TYPE);
  lcdDrawText(100, 2*FH, SD_IS_HC() ? STR_SDHC_CARD : STR_SD_CARD);

  lcdDrawText(MENUS_MARGIN_LEFT, 3*FH, STR_SD_SIZE);
  lcdDrawNumber(100, 3*FH, sdGetSize(), LEFT, 0, nullptr, "M");

  lcdDrawText(MENUS_MARGIN_LEFT, 4*FH, STR_SD_SECTORS);
#if defined(SD_GET_FREE_BLOCKNR)
  lcdDrawNumber(100, 4*FH, SD_GET_FREE_BLOCKNR()/1000, LEFT, 0, nullptr, "/");
  lcdDrawNumber(150, 4*FH, sdGetNoSectors()/1000, LEFT);
#else
  lcdDrawNumber(100, 4*FH, sdGetNoSectors()/1000, LEFT, 0, NULL, "k");
#endif

  lcdDrawText(MENUS_MARGIN_LEFT, 5*FH, STR_SD_SPEED);
  lcdDrawNumber(100, 5*FH, SD_GET_SPEED()/1000, LEFT, 0, NULL, "kb/s");

  return true;
}
#endif

#if 0
void onSdManagerMenu(const char * result)
{
  TCHAR lfn[FF_MAX_LFN+1];

  // TODO possible buffer overflows here!

  uint8_t index = menuVerticalPosition-menuVerticalOffset;
  char *line = reusableBuffer.sdmanager.lines[index];

  if (result == STR_SD_INFO) {
    pushMenu(menuRadioSdManagerInfo);
  }
  else if (result == STR_SD_FORMAT) {
    POPUP_CONFIRMATION(STR_CONFIRM_FORMAT);
  }
  else if (result == STR_PASTE) {
    f_getcwd(lfn, FF_MAX_LFN);
    // if destination is dir, copy into that dir
    if (IS_DIRECTORY(line)) {
      strcat(lfn, PSTR("/"));
      strcat(lfn, line);
    }
    if (strcmp(clipboard.data.sd.directory, lfn)) {  // prevent copying to the same directory
      POPUP_WARNING(sdCopyFile(clipboard.data.sd.filename, clipboard.data.sd.directory, clipboard.data.sd.filename, lfn));
      REFRESH_FILES();
    }
  }
  else if (result == STR_RENAME_FILE) {
    memcpy(reusableBuffer.sdmanager.originalName, line, sizeof(reusableBuffer.sdmanager.originalName));
    uint8_t fnlen = 0, extlen = 0;
    getFileExtension(line, 0, LEN_FILE_EXTENSION_MAX, &fnlen, &extlen);
    // write spaces to allow extending the length of a filename
    memset(line + fnlen - extlen, ' ', SD_SCREEN_FILE_LENGTH - fnlen + extlen);
    line[SD_SCREEN_FILE_LENGTH-extlen] = '\0';
    s_editMode = EDIT_MODIFY_STRING;
    editNameCursorPos = 0;
  }
  else if (result == STR_ASSIGN_BITMAP) {
    memcpy(g_model.header.bitmap, line, sizeof(g_model.header.bitmap));
    storageDirty(EE_MODEL);
  }
  else if (result == STR_ASSIGN_SPLASH) {
    f_getcwd(lfn, FF_MAX_LFN);
    sdCopyFile(line, lfn, SPLASH_FILE, BITMAPS_PATH);
  }
  else if (result == STR_VIEW_TEXT) {
    getSelectionFullPath(lfn);
    pushMenuTextView(lfn);
  }
}
#endif
