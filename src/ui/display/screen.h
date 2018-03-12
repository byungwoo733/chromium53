// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_DISPLAY_SCREEN_H_
#define UI_DISPLAY_SCREEN_H_

#include <vector>

#include "base/macros.h"
#include "ui/display/display_export.h"
#include "ui/gfx/native_widget_types.h"

namespace gfx {
class Point;
class Rect;
class Size;
}

namespace display {
class Display;
class DisplayObserver;

class DISPLAY_EXPORT ScreenDelegate {
 public:
  virtual gfx::Point GetCursorScreenPoint() = 0;
  virtual gfx::Size GetPreferredScreenSize(const gfx::Size requested_size) = 0;
  virtual int GetPreferredScreenRotationAsDegrees(
      int requested_rotation_as_degrees) = 0;
};

// A utility class for getting various info about screen size, displays,
// cursor position, etc.
//
// Note that this class does not represent an individual display connected to a
// computer -- see the Display class for that. A single Screen object exists
// regardless of the number of connected displays.
class DISPLAY_EXPORT Screen {
 public:
  Screen();
  virtual ~Screen();

  // Retrieves the single Screen object.
  static Screen* GetScreen();

  // Sets the global screen. NOTE: this does not take ownership of |screen|.
  // Tests must be sure to reset any state they install.
  static void SetScreenInstance(Screen* instance);

  // Returns the current absolute position of the mouse pointer.
  virtual gfx::Point GetCursorScreenPoint() = 0;

  // Returns true if the cursor is directly over |window|.
  virtual bool IsWindowUnderCursor(gfx::NativeWindow window) = 0;

  // Returns the window at the given screen coordinate |point|.
  virtual gfx::NativeWindow GetWindowAtScreenPoint(const gfx::Point& point) = 0;

  // Returns the number of displays.
  // Mirrored displays are excluded; this method is intended to return the
  // number of distinct, usable displays.
  virtual int GetNumDisplays() const = 0;

  // Returns the list of displays that are currently available.
  virtual std::vector<display::Display> GetAllDisplays() const = 0;

  // Returns the display nearest the specified window.
  // If the window is NULL or the window is not rooted to a display this will
  // return the primary display.
  virtual display::Display GetDisplayNearestWindow(
      gfx::NativeView view) const = 0;

  // Returns the display nearest the specified point. |point| should be in DIPs.
  virtual display::Display GetDisplayNearestPoint(
      const gfx::Point& point) const = 0;

  // Returns the display that most closely intersects the provided bounds.
  virtual display::Display GetDisplayMatching(
      const gfx::Rect& match_rect) const = 0;

  // Returns the primary display.
  virtual display::Display GetPrimaryDisplay() const = 0;

  // Adds/Removes display observers.
  virtual void AddObserver(DisplayObserver* observer) = 0;
  virtual void RemoveObserver(DisplayObserver* observer) = 0;

  // Converts |screen_rect| to DIP coordinates in the context of |view| clamping
  // to the enclosing rect if the coordinates do not fall on pixel boundaries.
  // If |view| is null, the primary display is used as the context.
  virtual gfx::Rect ScreenToDIPRectInWindow(gfx::NativeView view,
                                            const gfx::Rect& screen_rect) const;

  // Converts |dip_rect| to screen coordinates in the context of |view| clamping
  // to the enclosing rect if the coordinates do not fall on pixel boundaries.
  // If |view| is null, the primary display is used as the context.
  virtual gfx::Rect DIPToScreenRectInWindow(gfx::NativeView view,
                                            const gfx::Rect& dip_rect) const;

  // Currently only WebOS browser implementation uses delegate and thus default
  // implementation is empty
  virtual void SetDelegate(ScreenDelegate* delegate) {}

 private:
  DISALLOW_COPY_AND_ASSIGN(Screen);
};

Screen* CreateNativeScreen();

}  // namespace display

#endif  // UI_DISPLAY_SCREEN_H_