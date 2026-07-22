#pragma once
#include <Arduino.h>

/* Outcome of querying GitHub for the latest release (see fetchLatestRelease()
   in arduinoCode.ino). Kept in a header rather than defined inline in the
   .ino: Arduino's build step auto-generates a forward declaration for every
   function in the sketch and splices those in immediately after the last
   #include line, well before any struct/enum defined further down in the
   same .ino file. A function returning a type defined later in that file
   then gets an unresolvable prototype ("'OTAReleaseInfo' does not name a
   type"). Defining the type in a header included up top sidesteps the
   ordering problem, since the header's contents are expanded in place before
   the auto-generated prototype block that follows. */
enum OTAFetchStatus { OTA_FETCH_HTTP_ERROR, OTA_FETCH_BAD_JSON, OTA_FETCH_NO_RELEASE, OTA_FETCH_OK };

struct OTAReleaseInfo {
  OTAFetchStatus status;
  String version;
  String assetUrl; // empty if this release has no asset named OTA_ASSET_NAME
};
