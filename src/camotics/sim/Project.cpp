/******************************************************************************\

    CAMotics is an Open-Source simulation and CAM software.
    Copyright (C) 2011-2017 Joseph Coffland <joseph@cauldrondevelopment.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

\******************************************************************************/

#include "Project.h"
#include "Workpiece.h"
#include "Sweep.h"
#include "ToolSweep.h"
#include <gcode/ToolPath.h>


#include <gcode/Move.h>
#include <gcode/ToolTable.h>

#include <cbang/os/SystemUtilities.h>
#include <cbang/time/Time.h>
#include <cbang/log/Logger.h>
#include <cbang/xml/XMLWriter.h>
#include <cbang/xml/XMLReader.h>
#include <cbang/config/EnumConstraint.h>
#include <cbang/debug/Debugger.h>

using namespace std;
using namespace cb;
using namespace CAMotics;


Project::Project(Options &_options, const std::string &filename) :
  options(_options), filename(filename), workpieceMargin(5), watch(true),
  lastWatch(0), dirty(false) {

  options.setAllowReset(true);

  options.pushCategory("Project");
  options.add("units", "GCode::Units used in project measurement",
              new EnumConstraint<GCode::ToolUnits>)->setDefault("mm");
  options.popCategory();

  options.pushCategory("Renderer");
  options.add("resolution-mode", "Automatically compute a reasonable renderer "
              "grid resolution.  Valid values are 'low', 'medium', 'high', "
              "'manual'.  If 'manual' then 'resolution' will be used.",
              new EnumConstraint<ResolutionMode>)->setDefault("medium");
  options.addTarget("resolution", resolution, "Renderer grid resolution");
  options.addTarget("render-mode", mode, "Render surface generation mode.");
  options.popCategory();

  options.pushCategory("NC Files");
  options.addTarget("watch", watch, "Watch input files for changes and "
                    "automatically reload");
  options.add("nc-files", "TPL/GCode files")->setType(Option::STRINGS_TYPE);
  options.popCategory();

  options.pushCategory("Workpiece");
  options.add("automatic-workpiece", "Automatically compute a cuboid "
              "workpiece based on the tool path boundary");
  options.addTarget("workpiece-margin", workpieceMargin,
                    "Percent margin around automatic workpiece");
  options.addTarget("workpiece-min", workpieceMin,
                    "Minimum bound of cuboid workpiece");
  options.addTarget("workpiece-max", workpieceMax,
                    "Maximum bound of cuboid workpiece");
  options.popCategory();

  if (!filename.empty()) load(filename);
}


Project::~Project() {}


void Project::markDirty() {
  dirty = true;
}


void Project::setFilename(const string &_filename) {
  if (_filename.empty() || filename == _filename) return;
  filename = _filename;
  markDirty();
}


string Project::getDirectory() const {
  return filename.empty() ? SystemUtilities::getcwd() :
    SystemUtilities::dirname(filename);
}


void Project::setUnits(GCode::ToolUnits units) {
  if (units == getUnits()) return;
  options["units"].set(units.toString());
  markDirty();
}


GCode::ToolUnits Project::getUnits() const {
  return GCode::ToolUnits::parse(options["units"]);
}


ResolutionMode Project::getResolutionMode() const {
  return ResolutionMode::parse(options["resolution-mode"]);
}


void Project::setResolutionMode(ResolutionMode x) {
  if (x == getResolutionMode()) return;

  options["resolution-mode"].set(x.toString());
  markDirty();
  updateResolution();
}


void Project::setResolution(double x) {
  if (x == getResolution()) return;

  options["resolution"].set(x);

  if (getResolutionMode() == ResolutionMode::RESOLUTION_MANUAL) markDirty();
}


double Project::computeResolution(ResolutionMode mode, cb::Rectangle3D bounds) {
  if (mode == ResolutionMode::RESOLUTION_MANUAL || bounds == cb::Rectangle3D())
    return 1;

  double divisor;
  switch (mode) {
  case ResolutionMode::RESOLUTION_LOW: divisor = 100000; break;
  case ResolutionMode::RESOLUTION_HIGH: divisor = 5000000; break;
  case ResolutionMode::RESOLUTION_VERY_HIGH: divisor = 10000000; break;
  default: divisor = 250000; break; // Medium
  }

  return pow(bounds.getVolume() / divisor, 1.0 / 3.0);
}


void Project::updateResolution() {
  ResolutionMode mode = getResolutionMode();

  if (mode != ResolutionMode::RESOLUTION_MANUAL)
    setResolution(computeResolution(mode, getWorkpieceBounds()));
}


void Project::load(const string &_filename) {
  setFilename(_filename);

  if (SystemUtilities::exists(_filename)) {
    XMLReader reader;
    reader.addFactory("tool_table", &tools);
    reader.read(filename, &options);

    // Default workpiece
    if (!options["automatic-workpiece"].hasValue())
      options["automatic-workpiece"].
        setDefault(workpieceMin.empty() && workpieceMax.empty());

    // Load NC files
    files.clear();
    Option::strings_t ncFiles = options["nc-files"].toStrings();
    for (unsigned i = 0; i < ncFiles.size(); i++) {
      string relPath = decodeFilename(ncFiles[i]);
      addFile(SystemUtilities::absolute(getDirectory(), relPath));
    }
  }

  workpiece = getWorkpieceBounds();

  markClean();
}


void Project::save(const string &_filename) {
  setFilename(_filename);

  // Set nc-files option
  options["nc-files"].reset();
  for (files_t::iterator it = files.begin(); it != files.end(); it++)
    options["nc-files"].append(encodeFilename((*it)->getRelativePath()));

  SmartPointer<iostream> stream = SystemUtilities::open(filename, ios::out);
  XMLWriter writer(*stream, true);

  writer.startElement("camotics");
  writer.comment("Note, all values are in mm regardless of 'units' option.");
  options.write(writer, 0);
  tools.write(writer);
  writer.endElement("camotics");

  markClean();
}


const SmartPointer<NCFile> &Project::getFile(unsigned index) const {
  unsigned count = 0;

  for (iterator it = begin(); it != end(); it++)
    if (count++ == index) return *it;

  THROWS("Invalid file index " << index);
}


SmartPointer<NCFile> Project::findFile(const string &filename) const {
  string abs = SystemUtilities::absolute(filename);
  for (iterator it = begin(); it != end(); it++)
    if ((*it)->getAbsolutePath() == abs) return *it;
  return 0;
}


void Project::addFile(const string &filename) {
  string abs = SystemUtilities::absolute(filename);
  if (!findFile(abs).isNull()) return; // Duplicate

  files.push_back(new NCFile(*this, abs));
  markDirty();
}


void Project::removeFile(unsigned index) {
  unsigned count = 0;
  for (files_t::iterator it = files.begin(); it != files.end(); it++)
    if (count++ == index) {
      files.erase(it);
      markDirty();
      break;
    }
}


bool Project::checkFiles() {
  bool changed = false;

  if (watch && lastWatch < Time::now()) {
    for (iterator it = begin(); it != end(); it++)
      if ((*it)->changed()) {
        LOG_INFO(1, "File changed: " << (*it)->getRelativePath());
        changed = true;
      }

    lastWatch = Time::now();
  }

  return changed;
}


void Project::updateAutomaticWorkpiece(GCode::ToolPath &path) {
  if (!getAutomaticWorkpiece()) return;
  setAutomaticWorkpiece(true);
  cb::Rectangle3D wpBounds;

  // Guess workpiece bounds from cutting moves
  vector<SmartPointer<Sweep> > sweeps;
  vector<cb::Rectangle3D> bboxes;

  for (unsigned i = 0; i < path.size(); i++) {
    const GCode::Move &move = path.at(i);

    if (move.getType() == GCode::MoveType::MOVE_RAPID) continue;

    int tool = move.getTool();
    if (tool < 0) continue;

    if (sweeps.size() <= (unsigned)tool) sweeps.resize(tool + 1);
    if (sweeps[tool].isNull())
      sweeps[tool] = ToolSweep::getSweep(tools.get(tool));

    sweeps[tool]->getBBoxes(move.getStartPt(), move.getEndPt(), bboxes, 0);
  }

  for (unsigned i = 0; i < bboxes.size(); i++) wpBounds.add(bboxes[i]);

  if (wpBounds == cb::Rectangle3D()) return;

  // Start from z = 0
  cb::Vector3D bMin = wpBounds.getMin();
  cb::Vector3D bMax = wpBounds.getMax();
  wpBounds = cb::Rectangle3D(bMin, cb::Vector3D(bMax.x(), bMax.y(), 0));

  // At least 2mm thick
  if (wpBounds.getHeight() < 2)
    wpBounds.add(cb::Vector3D(bMin.x(), bMin.y(), bMin.z() - 2));

  if (wpBounds.isReal()) {
    // Margin
    cb::Vector3D margin =
      wpBounds.getDimensions() * getWorkpieceMargin() / 100.0;
    wpBounds.add(wpBounds.getMin() - margin);
    wpBounds.add(wpBounds.getMax() + cb::Vector3D(margin.x(), margin.y(), 0));

    setWorkpieceBounds(wpBounds);
  }
}


bool Project::getAutomaticWorkpiece() const {
  return (options["automatic-workpiece"].hasValue() &&
          options["automatic-workpiece"].toBoolean()) ||
    (workpieceMin.empty() && workpieceMax.empty());
}


void Project::setAutomaticWorkpiece(bool x) {
  if (getAutomaticWorkpiece() != x) markDirty();
  options["automatic-workpiece"].set(x);
}


void Project::setWorkpieceMargin(double x) {
  if (getWorkpieceMargin() == x) return;
  options["workpiece-margin"].set(x);
  markDirty();
}


void Project::setWorkpieceBounds(const cb::Rectangle3D &bounds) {
  options["workpiece-min"].set(bounds.getMin().toString());
  options["workpiece-max"].set(bounds.getMax().toString());
  updateResolution();
  if (!getAutomaticWorkpiece()) markDirty();
  workpiece = bounds;
}


cb::Rectangle3D Project::getWorkpieceBounds() const {
  cb::Vector3D wpMin =
    workpieceMin.empty() ? cb::Vector3D() : cb::Vector3D(workpieceMin);
  cb::Vector3D wpMax =
    workpieceMax.empty() ? cb::Vector3D() : cb::Vector3D(workpieceMax);
  return cb::Rectangle3D(wpMin, wpMax);
}


string Project::encodeFilename(const string &filename) {
  string result;

  for (unsigned i = 0; i < filename.size(); i++)
    switch (filename[i]) {
    case '\t': result += "%07"; break;
    case '\n': result += "%0A"; break;
    case '\v': result += "%0B"; break;
    case '\r': result += "%0D"; break;
    case '%': result += "%25"; break;
    case ' ': result += "%20"; break;
    default: result += filename[i]; break;
    }

  return result;
}


string Project::decodeFilename(const string &filename) {
  string result;

  for (unsigned i = 0; i < filename.size(); i++)
    if (filename[i] == '%' && i < filename.size() - 2) {
      result += (char)String::parseU8("0x" + filename.substr(i + 1, 2));
      i += 2;

    } else result += filename[i];

  return result;
}
