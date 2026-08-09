// The version, in one place.
//
// It used to be written out twice -- once here for the panel and once in
// wrinject.rc for the executable's Properties -> Details tab -- which is one
// place too many for something that has to be bumped by hand every release.
// The two had already drifted once.
//
// This header holds nothing but #define lines on purpose: rc.exe preprocesses
// .rc files itself and will not parse C++ declarations, so a version header
// that is safe for BOTH the compiler and the resource compiler cannot contain
// anything else. Do not add a struct or a typedef here.
#ifndef WR_VERSION_H
#define WR_VERSION_H

#define WRLINES_VERSION     "0.4.5"
#define WRLINES_VERSION_NUM 0, 4, 5, 0

#endif
