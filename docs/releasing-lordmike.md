# LordMike WLED release procedure

Push a version tag to build firmware and create a draft GitHub release. Update its notes, then ask the user before publishing. Commands below use the fork remote `lordmike` and upstream remote `wled`.

## Release checklist

1. **Select a verified commit and version.** Fetch fork tags and upstream, then confirm WLED CI passed for the exact commit. Read the original WLED version from `package.json` at the upstream commit incorporated into that release. Keep its major, minor, and patch numbers; omit generic `dev` if desired, but preserve `beta` or `rc`.

   Append `lordmike.N`: upstream `16.0.1-dev` becomes `v16.0.1-lordmike.2`; `16.1.0-rc1` becomes `v16.1.0-rc1.lordmike.1`. Increment `N` above all existing fork tags for the same numeric WLED version, including drafts and failed releases. Start at `1` when the numeric WLED version changes. Never move or reuse a release tag.

   WLED CI's current `branches: ['*']` filter excludes names containing `/`. Validate through a branch without a slash or a pull request within the fork.

2. **Push the tag.** Keep **LordMike Release CI** enabled and the upstream **WLED Release CI** disabled in GitHub. Replace these example values with the selected commit and next unused tag:

   ```powershell
   $releaseCommit = "FULL_VERIFIED_COMMIT_SHA"
   $releaseTag = "v16.0.1-lordmike.2"
   git tag $releaseTag $releaseCommit
   git push lordmike "refs/tags/$releaseTag"
   ```

   CI takes the version from the tag; no `package.json` edit is needed.

3. **Wait for release CI.** Find the tag's run, then substitute its numeric ID for `RUN_ID`:

   ```powershell
   gh run list --repo LordMike/WLED --workflow lordmike-release.yml --branch $releaseTag
   gh run watch RUN_ID --repo LordMike/WLED --exit-status
   ```

   Require the complete workflow to succeed. Confirm the draft contains `WLED_<version>_PXP.bin` and `WLED_<version>_PXP_AHT10_INA226.bin`.

4. **Update the GitHub draft.** Replace placeholder notes with changes since the previous published fork release: upstream and PXP improvements, compatibility changes, verified tests, and known limitations. Cite relevant commits or comparisons; distinguish CI results from hardware testing. Preserve useful existing notes and assets.

   Write notes to a temporary Markdown file, set `$releaseNotesPath` to its path, and update GitHub while retaining draft status:

   ```powershell
   gh release edit $releaseTag --repo LordMike/WLED --title $releaseTag --notes-file $releaseNotesPath --draft=true
   gh release view $releaseTag --repo LordMike/WLED
   ```

   Verify the saved notes and assets. Keep upstream `beta` or `rc` builds marked as GitHub prereleases; the workflow does not set that flag automatically.

5. **Ask before removing draft status.** Show the updated release link and summarize its notes, then ask: **"Should I remove draft status and publish this release?"** Wait for an explicit yes for this release. Permission to prepare the release or update notes is not publication approval; without an answer, leave it as a draft.

   Only after approval:

   ```powershell
   gh release edit $releaseTag --repo LordMike/WLED --draft=false
   gh release view $releaseTag --repo LordMike/WLED --json isDraft,isPrerelease,body,assets,url
   ```

   Confirm `isDraft` is false and the reviewed notes, assets, and prerelease status remain correct.

Sources: [release workflow](../.github/workflows/lordmike-release.yml), [shared build workflow](../.github/workflows/build.yml), [release matrix](../.github/platformio_lordmike_release.ini.template), [fork environments](../platformio_override.ini).
