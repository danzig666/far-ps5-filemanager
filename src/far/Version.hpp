#pragma once

#define PS5FM_VERSION_MAJOR 1
#define PS5FM_VERSION_MINOR 0
#define PS5FM_VERSION_REVISION 1
#define PS5FM_VERSION_BUILD 2

#define PS5FM_STR2(x) #x
#define PS5FM_STR(x) PS5FM_STR2(x)
#define PS5FM_VERSION_STRING                                                        \
  PS5FM_STR(PS5FM_VERSION_MAJOR) "." PS5FM_STR(PS5FM_VERSION_MINOR) "." PS5FM_STR( \
      PS5FM_VERSION_REVISION)

// Oldest Far build this plugin is built and tested for.
#define PS5FM_MIN_FAR_BUILD 6638
