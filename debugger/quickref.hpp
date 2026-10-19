/*
  SPDX-License-Identifier: GPL-2.0-or-later
  ONE NES emulator
  Copyright (C) 1999-2026 Chris Teague

  ONE is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  ONE is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with ONE; if not, see <https://www.gnu.org/licenses/>.
*/
// Help > Quick Reference: the one-debugger quick reference card (docs/quickref/one-debugger-quickref.pdf), opened in
// the desktop's PDF viewer.
#pragma once
#include <string>

// The PDF: the installed copy (bin/../share/doc/one/), else the source tree's, else one made by the build
// (cmake --build <dir> --target quickref); "" if there is none
std::string quickReferencePath();

// Open it with xdg-open, without waiting for the viewer (its output goes nowhere: not over the text UI). Returns
// the line for the command window: what was opened, or why not.
std::string openQuickReference();
