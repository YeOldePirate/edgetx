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

#include "menu_model.h"

#include "translations.h"
#include "view_channels.h"
#include "model_curves.h"
#include "model_flightmodes.h"
#include "model_gvars.h"
#include "model_heli.h"
#include "model_inputs.h"
#include "model_logical_switches.h"
#include "model_mixer_scripts.h"
#include "model_mixes.h"
#include "model_outputs.h"
#include "model_setup.h"
#include "model_telemetry.h"
#include "opentx.h"
#include "special_functions.h"

ModelMenu::ModelMenu():
  TabsGroup(ICON_MODEL)
{
  addTab(new ModelSetupPage());
#if defined(HELI)
  addTab(new ModelHeliPage());
#endif
#if defined(FLIGHT_MODES)
  addTab(new ModelFlightModesPage());
#endif
  addTab(new ModelInputsPage());
  addTab(new ModelMixesPage());
  addTab(new ModelOutputsPage());
  addTab(new ModelCurvesPage());
#if defined(GVARS)
  addTab(new ModelGVarsPage());
#endif
  addTab(new ModelLogicalSwitchesPage());
  addTab(new SpecialFunctionsPage(g_model.customFn));
#if defined(LUA_MODEL_SCRIPTS)
  addTab(new ModelMixerScriptsPage());
#endif
  addTab(new ModelTelemetryPage());

  addButton(&header);
}

void ModelMenu::addButton(TabsGroupHeader* header)
{
  OpenTxTheme::instance()->createTextButton(
      header, {LCD_W / 2 + 5, MENU_TITLE_TOP, LCD_W / 2 - 5, MENU_TITLE_HEIGHT},
      STR_OPEN_CHANNEL_MONITORS, [=]() {
        calledFromModel = 1;
        retTab = header->getCarousel()->getCurrentIndex();
        // TRACE("currentTab=%d  %s", calledFromModel, typeid(*currentTab).name());
        new ChannelsViewMenu();
        deleteLater();
        return 0;
      });
}
