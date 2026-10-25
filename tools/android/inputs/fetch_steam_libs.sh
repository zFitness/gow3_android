#!/usr/bin/env bash
# Libraries for the app's Steam sign-in, which downloads Lossless.dll from the player's own Lossless
# Scaling (Steam app 993090) for Lossless Scaling frame generation (SteamDllDialog.java):
# JavaSteam (MIT, a Java port of SteamKit2) with its dependencies, xz and zstd-jni (Android build:
# its arm64 library) to unpack depot chunks, Spongy Castle (Bouncy Castle licence: the crypto provider
# JavaSteam uses on Android, as Android's own "BC" provider lacks AES/ECB) to decrypt manifests, and
# ZXing core (Apache 2.0) to draw the sign-in QR code.
# Resolved with a pinned Apache Maven into .local-deps/android/steam-libs/{jars,jni}; the
# android-launcher Gradle build dexes the jars into the APK.
set -euo pipefail
cd -- "$(dirname -- "$0")/../../.."
deps=$PWD/.local-deps/android
out=$deps/steam-libs
MAVEN_VERSION=3.9.11
MAVEN_SHA512=bcfe4fe305c962ace56ac7b5fc7a08b87d5abd8b7e89027ab251069faebee516b0ded8961445d6d91ec1985dfe30f8153268843c89aa392733d1a3ec956c9978
JAVASTEAM=1.8.0 XZ=1.12 ZSTD=1.5.7-6 ZXING=3.5.4 SPONGY=1.58.0.0
if [[ ! -x $deps/maven/bin/mvn ]]; then
    curl -sfL -o "$deps/maven.tar.gz" \
        "https://archive.apache.org/dist/maven/maven-3/$MAVEN_VERSION/binaries/apache-maven-$MAVEN_VERSION-bin.tar.gz"
    echo "$MAVEN_SHA512  $deps/maven.tar.gz" | sha512sum -c --quiet -
    mkdir -p "$deps/maven" && tar -xzf "$deps/maven.tar.gz" -C "$deps/maven" --strip-components=1
    rm "$deps/maven.tar.gz"
fi
work=$(mktemp -d); trap 'rm -rf "$work"' EXIT
cat > "$work/pom.xml" <<EOF
<project xmlns="http://maven.apache.org/POM/4.0.0">
  <modelVersion>4.0.0</modelVersion>
  <groupId>local.bloodborne</groupId><artifactId>steam-libs</artifactId><version>1</version>
  <dependencies>
    <dependency><groupId>in.dragonbra</groupId><artifactId>javasteam</artifactId><version>$JAVASTEAM</version></dependency>
    <dependency><groupId>org.tukaani</groupId><artifactId>xz</artifactId><version>$XZ</version></dependency>
    <dependency><groupId>com.github.luben</groupId><artifactId>zstd-jni</artifactId><version>$ZSTD</version><type>aar</type></dependency>
    <dependency><groupId>com.madgag.spongycastle</groupId><artifactId>prov</artifactId><version>$SPONGY</version>
      <exclusions><exclusion><groupId>junit</groupId><artifactId>junit</artifactId></exclusion></exclusions></dependency>
    <dependency><groupId>com.google.zxing</groupId><artifactId>core</artifactId><version>$ZXING</version></dependency>
  </dependencies>
</project>
EOF
rm -rf "$out"; mkdir -p "$out/jars" "$out/jni"
"$deps/maven/bin/mvn" -q -B -f "$work/pom.xml" -Dmaven.repo.local="$deps/m2" \
    dependency:copy-dependencies -DoutputDirectory="$work/deps" -DincludeScope=runtime
for file in "$work"/deps/*; do
    case $file in
        *.jar) cp "$file" "$out/jars/" ;;
        *.aar) # zstd-jni for Android: its classes and the arm64 JNI library
            unzip -q -o "$file" classes.jar 'jni/arm64-v8a/*' -d "$work/aar"
            cp "$work/aar/classes.jar" "$out/jars/zstd-jni-$ZSTD.jar"
            # Gradle jniLibs layout: <abi>/libzstd-jni.so (zstd-jni loads it as "zstd-jni").
            mkdir -p "$out/jni/arm64-v8a"
            cp "$work"/aar/jni/arm64-v8a/*.so "$out/jni/arm64-v8a/libzstd-jni.so" ;;
    esac
done
ls "$out/jars" > "$out/jars.txt"
echo "Steam libraries: $(wc -l < "$out/jars.txt") jars, $(ls "$out/jni") in $out"
