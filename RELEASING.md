# Releasing

Releases are tagged with the version, such as `0.10.0-16kb.1`. Pushing a tag that matches
`*-16kb.*` starts the `release` workflow, which builds and checks everything and creates a draft
GitHub release.

1. Make sure the branch you release from is green: the `build` workflow runs the JVM tests on all
   four platforms with CheckJNI, builds the Android library, checks its 16 KB alignment and runs
   the instrumentation tests on an API 21 emulator and a 16 KB emulator.

2. Set `VERSION_NAME` in `gradle.properties` to the release version, without `-SNAPSHOT`. It is
   the only place the version is defined.

3. Update `CHANGELOG.md`:
   1. Rename the `Unreleased` section to `[X.Y.Z-16kb.N] - YYYY-MM-DD`.
   2. Add the link for the new header at the bottom, and point `Unreleased` at the new tag.
   3. Add a new, empty `Unreleased` section at the top.

4. Update the versions in `README.md`.

5. Check the release locally:

   ```sh
   scripts/check-release-version.sh X.Y.Z-16kb.N
   ```

6. Commit and tag:

   ```sh
   git commit -am "Prepare version X.Y.Z-16kb.N"
   git tag -am "Version X.Y.Z-16kb.N" X.Y.Z-16kb.N
   ```

7. Set `VERSION_NAME` to the next `-SNAPSHOT` version and commit:

   ```sh
   git commit -am "Prepare next development version"
   ```

8. Push the commits and only the new tag:

   ```sh
   git push
   git push origin X.Y.Z-16kb.N
   ```

9. When the `release` workflow finishes, review the draft release. It contains
   `quickjs-android-X.Y.Z-16kb.N.aar`, `quickjs-jvm-X.Y.Z-16kb.N.jar` with its sources and Javadoc
   JARs, a POM for each, and `SHA256SUMS`. The workflow has already checked that every file and
   POM carries the tag's version. Publish the release.

This fork isn't published to Maven Central; consumers use the release files or build from source
(see the README).
