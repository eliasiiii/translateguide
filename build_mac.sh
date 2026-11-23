#!/bin/sh
# Needs macOS + Xcode command line tools (no Theos needed). UNTESTED — this is
# the first time this exact two-step Swift+ObjC++ build has been attempted.
set -e
SDK=iphoneos
MIN=18.0   # TranslationSession requires iOS 18+

echo "== 1/3: compiling Swift, emitting an Objective-C header =="
xcrun -sdk $SDK swiftc -target arm64-apple-ios$MIN \
  -emit-object -emit-objc-header-path UniversalTranslate_Swift.h \
  -module-name UniversalTranslate \
  -parse-as-library \
  -o TranslationBridge.o \
  TranslationBridge.swift

echo "== 2/3: compiling the Objective-C++ hook against that header =="
xcrun -sdk $SDK clang++ -arch arm64 -miphoneos-version-min=$MIN -std=c++20 -fobjc-arc \
  -c UniversalTranslate.mm -o UniversalTranslate.o

echo "== 3/3: linking =="
xcrun -sdk $SDK clang++ -arch arm64 -miphoneos-version-min=$MIN \
  -dynamiclib -framework Foundation -framework UIKit -framework SwiftUI \
  -framework Translation -framework JavaScriptCore \
  -install_name @rpath/UniversalTranslate.dylib \
  -o UniversalTranslate.dylib TranslationBridge.o UniversalTranslate.o
file UniversalTranslate.dylib
