# Maintaining the GitHub wiki

The Markdown files in `docs/wiki/` are the source for the project's GitHub wiki. Publish all
six Markdown files at the root of the separate wiki repository; `_Sidebar.md` provides navigation.
Copy `docs/images/` to `assets/` in the wiki repository too, so the installation images resolve.
Do not copy this maintenance guide into the wiki.

Repository: `https://github.com/agustinmorantes/FF7RebirthLegacyGPUCompat.wiki.git`

GitHub requires an initial page created through its Wiki tab before the wiki can be cloned.
After that, clone the wiki, copy the files from `docs/wiki/`, review the diff and commit there.
Publishing changes requires updating that separate wiki repository; a commit to the main
source repository alone does not update the wiki.

Keep installation steps consistent with `README.md` and the bundled readme in
`tools/package_release.py`. Keep detailed runtime internals in `docs/how-it-works.md` and link
to them from the wiki. Avoid adding generated shaders, local logs or private release notes.

The wiki follows the source repository's visibility. Publish it while the repository is private;
do not change repository visibility as part of a documentation update.

GitHub reference: [Adding or editing wiki pages](https://docs.github.com/en/communities/documenting-your-project-with-wikis/adding-or-editing-wiki-pages).
