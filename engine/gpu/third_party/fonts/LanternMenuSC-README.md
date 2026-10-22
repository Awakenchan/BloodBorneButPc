# Chinese menu font

`LanternMenuSC.ttf` is a regular-weight subset of Noto Sans SC, renamed **Lantern
Menu SC**. It contains the non-ASCII characters used by the Simplified Chinese
Insert menu and its language selector. The existing DejaVu Sans font supplies
Latin and Cyrillic characters; the subset is merged into that font at runtime.
The subset is licensed under the SIL Open Font License 1.1; see
`LICENSE-LanternMenuSC.txt`.

Source: [Google Fonts, revision a85815a42757630ce188fdad368c2dfc444d4773](https://github.com/google/fonts/tree/a85815a42757630ce188fdad368c2dfc444d4773/ofl/notosanssc).

- Source file: `NotoSansSC[wght].ttf`
- SHA-256: `a3041811a78c361b1de50f953c805e0244951c21c5bd412f7232ef0d899af0da`
- Weight: 400
- Subset: code points in `LanternMenuSC-glyphs.txt`
- Generated with fontTools 4.67.0 (`varLib.instancer` followed by `subset`),
  retaining all name records and changing name IDs 1/3/4/6/16 to the new family
  and PostScript names. No fontTools dependency is needed to build the engine.
- Contributed by 123456qewret (LanternEngine pull request 71; 41 characters added in pull request 73).

When adding translations, include any new characters in the subset (the menu shows a
placeholder for a character outside it). Redistributed engine packages must retain the font
license (`licenses/LanternMenuSC-OFL.txt`).
