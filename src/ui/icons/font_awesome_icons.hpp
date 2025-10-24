// Font Awesome 6 Free Solid Icon Definitions
// Based on IconFontCppHeaders: https://github.com/juliettef/IconFontCppHeaders
// FontAwesome License: https://fontawesome.com/license/free (Icons: CC BY 4.0, Fonts: SIL OFL 1.1)

#pragma once

// Font file name and configuration
#define FONT_ICON_FILE_NAME_FAS "fa-solid-900.ttf"

// FontAwesome 6 icon range (all solid icons)
#define ICON_MIN_FA 0xf000
#define ICON_MAX_FA 0xf8ff

// Helper macro to convert icon code to UTF-8 string
#define ICON_FA_STR(x) x

// Viewport Toolbar Icons - carefully selected for clarity
#define ICON_FA_ARROWS_UP_DOWN_LEFT_RIGHT "\xef\x81\x87"  // f047 - 3D axis gizmo
#define ICON_FA_BORDER_ALL                "\xef\xa1\x8c"  // f84c - grid pattern
#define ICON_FA_DRAW_POLYGON              "\xef\x97\xae"  // f5ee - wireframe/polygon

// Alternative icons (commented out, but available if you want to try different icons)
#define ICON_FA_CUBE                   "\xef\x86\xb2"  // f1b2 - 3D cube
// #define ICON_FA_VECTOR_SQUARE          "\xef\x97\x8b"  // f5cb - vector square for wireframe
// #define ICON_FA_TH                     "\xef\x80\x8a"  // f00a - simple grid (3x3)
// #define ICON_FA_GRID_2                 "\xe1\x95"      // e195 - modern grid icon (FA6)

// Additional useful viewport icons for future use
#define ICON_FA_EYE                       "\xef\x81\xae"  // f06e - visibility toggle
#define ICON_FA_EYE_SLASH                 "\xef\x81\xb0"  // f070 - hide
#define ICON_FA_LIGHTBULB                 "\xef\x83\xab"  // f0eb - lighting
#define ICON_FA_SUN                       "\xef\x86\x85"  // f185 - sun/light
#define ICON_FA_CAMERA                    "\xef\x80\xb0"  // f030 - camera
#define ICON_FA_VIDEO                     "\xef\x80\xbd"  // f03d - video/render
#define ICON_FA_ARROWS_ALT                "\xef\x82\xb2"  // f0b2 - move gizmo
#define ICON_FA_SYNC_ALT                  "\xef\x8b\xb1"  // f2f1 - rotate gizmo
#define ICON_FA_EXPAND_ALT                "\xef\x82\xa5"  // f065 - scale gizmo
#define ICON_FA_COGS                      "\xef\x88\x85"  // f085 - settings
#define ICON_FA_SLIDERS_H                 "\xef\x87\x9e"  // f1de - sliders/controls

// Side Panel Tab Icons
#define ICON_FA_WRENCH                    "\xef\x80\xad"  // f0ad - properties/tools
#define ICON_FA_GLOBE                     "\xef\x83\xac"  // f0ac - world/scene
#define ICON_FA_ATOM                      "\xef\x97\x92"  // f5d2 - physics/particles
#define ICON_FA_IMAGE                     "\xef\x80\xbe"  // f03e - render/image
#define ICON_FA_TREE                      "\xef\x86\xbb"  // f1bb - environment/nature
#define ICON_FA_PAINT_BRUSH               "\xef\x87\xbc"  // f1fc - materials/shaders

// Common UI icons
#define ICON_FA_TIMES                     "\xef\x80\x8d"  // f00d - close/cancel
#define ICON_FA_CHECK                     "\xef\x80\x8c"  // f00c - confirm/check
#define ICON_FA_PLUS                      "\xef\x81\xa7"  // f067 - add
#define ICON_FA_MINUS                     "\xef\x81\xa8"  // f068 - remove
#define ICON_FA_COG                       "\xef\x80\x93"  // f013 - settings
#define ICON_FA_FOLDER_OPEN               "\xef\x81\xbc"  // f07c - folder
#define ICON_FA_SAVE                      "\xef\x83\x87"  // f0c7 - save
#define ICON_FA_PLAY                      "\xef\x81\x8b"  // f04b - play
#define ICON_FA_PAUSE                     "\xef\x81\x8c"  // f04c - pause
#define ICON_FA_STOP                      "\xef\x81\x8d"  // f04d - stop
#define ICON_FA_CIRCLE                    "\xef\x84\x91"  // f111 - circle (empty actor)
#define ICON_FA_PALETTE                   "\xef\x94\xbf"  // f53f - palette (material)
