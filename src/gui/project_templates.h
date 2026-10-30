/*
 * File:        project_templates.h
 * Module:      gui
 * Purpose:     Built-in project and section templates for File > New and the
 *              section list's typed add menu
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 Simon Inns
 */

#pragma once

#include "videosynth/model.h"

namespace videosynth::gui {

// Thread-safety: all functions in this module are thread-safe pure functions.

// Returns a minimal project for `standard` that passes structural validation
// (ProjectValidator without a source probe): one progressive section sourced
// from the bundled colour-bar EXR so it previews immediately. This is the
// starting point File > New / the New Project dialog hands to the editors.
Project MakeDefaultProject(Standard standard);

// Convenience wrapper for MakeDefaultProject(Standard::kPal).
Project MakeDefaultPalProject();

// The bundled asset raster subfolder for `standard`: "720x576" for PAL,
// "720x486" for NTSC/PAL-M (System-M). Bundled assets are laid out as
// {bundled}/<type>/<raster>/<file>, so the section editor composes a built-in
// source path from this raster plus the asset type and the chosen filename.
std::string BundledRaster(Standard standard);

// The bundled colour-bar source path MakeDefaultProject seeds for `standard`,
// referenced through the {bundled} logical asset root. Exposed so callers can
// detect and remap the seeded default when the project standard changes.
std::string DefaultBundledSource(Standard standard);

// Rewrites any section source that is still one of the bundled default
// colour-bar EXRs so it matches `standard`'s active raster (720x576 for PAL,
// 720x486 for NTSC/PAL-M). Sources the user has chosen are left untouched.
// Returns the number of sections changed. Used when the New Project dialog
// switches the standard on the scratch document so the seeded section stays
// previewable instead of retaining a raster the new standard rejects.
int RemapBundledDefaultSources(Project* project, Standard standard);

// Plain progressive section (no laserdisc VBI codes). `ordinal` seeds the
// default name ("Section <ordinal>"); `standard` selects the bundled
// colour-bar default source for the matching active raster.
Section MakeProgressiveSectionTemplate(int ordinal, Standard standard);

// The bundled 24000/1001 film clip MakeFilmSectionTemplate seeds, referenced
// through the {bundled} logical asset root (525-line raster).
std::string FilmBundledSource();

// Film section shown with 2:3 pulldown (cadence pattern "2:3", phase A,
// restarting the cadence), playing every frame of the bundled film clip.
// Offered for 525-line projects only (SupportsFilmPulldown). `ordinal` seeds
// the default name ("Film <ordinal>"); `standard` must be NTSC or PAL-M.
Section MakeFilmSectionTemplate(int ordinal, Standard standard);

// Project-wide line_injections for a laserdisc project: the CAV disc format
// and, for NTSC/PAL-M, the mandatory virs VITS colour reference
// (IEC 60857 §9.1.3). Pair this with the laserdisc section templates below,
// whose sections carry only their per-section biphase codes.
ProjectLineInjections MakeLaserdiscLineInjections(Standard standard);

// Laserdisc section templates modelled on the projects/ fixtures. Each carries
// laserdisc code injection IEC 60856/60857 expects for its section type; NTSC
// and PAL-M additionally get the mandatory FM white flag. The disc format and
// VITS set are project-wide (see MakeLaserdiscLineInjections). Durations
// default to the IEC track-pitch minimums the validator checks (lead-in 938,
// lead-out 1250).
Section MakeLaserdiscLeadInSectionTemplate(Standard standard);
Section MakeLaserdiscProgrammeSectionTemplate(Standard standard);
Section MakeLaserdiscLeadOutSectionTemplate(Standard standard);

// Copy of `section` renamed "<name> (copy)" (then "(copy 2)", … if needed)
// so duplicates stay distinguishable in the section list.
Section MakeDuplicateSection(const Section& section,
                             const std::vector<Section>& existing_sections);

}  // namespace videosynth::gui
