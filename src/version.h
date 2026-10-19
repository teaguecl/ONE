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
// ONE's version, e.g. "0.1.0" for a release or "0.1.0+g1a2b3c4" (with "-dirty" for local changes) for any other build.
// The number is set in CMakeLists.txt (project VERSION); cmake/version.cmake adds the commit.
#ifndef version_h_defined
#define version_h_defined

#ifdef __cplusplus
extern "C" {
#endif

const char *one_version(void);

#ifdef __cplusplus
}
#endif

#endif
