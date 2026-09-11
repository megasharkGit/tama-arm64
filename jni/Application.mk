# Compilar APENAS para ARM64 (o objetivo do projeto).
APP_ABI := arm64-v8a

# Nível mínimo de API. minSdk do APK é 26; 21+ é seguro para arm64.
APP_PLATFORM := android-26

# STL: o motor em C puro não precisa de C++.
APP_STL := none

APP_OPTIM := release
