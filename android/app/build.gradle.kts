plugins { id("com.android.application") }

android {
    namespace = "ch.plum.tmturbo.recomp"
    compileSdk = 35
    ndkVersion = "27.3.13750724"   // the NDK tools/build_android_so.sh uses; AGP strips libtmturbo.so with it
    defaultConfig {
        applicationId = "ch.plum.tmturbo.recomp"
        minSdk = 29
        targetSdk = 35
        versionCode = 18
        versionName = "0.14.0"
        ndk { abiFilters += "arm64-v8a" }
    }
    // libtmturbo.so (runtime + lifted game code) is prebuilt locally with the
    // NDK from the user's own TrackmaniaTurbo.exe; see port/RECOMP_STATUS.md.
    packaging { jniLibs { useLegacyPackaging = true } }
    buildTypes {
        getByName("debug") { isDebuggable = true }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}
