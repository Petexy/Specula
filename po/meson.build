# Compile and install the gettext message catalogs listed in LINGUAS.
# The translatable sources are enumerated in POTFILES; `specula.pot` is the
# extraction template and each <lang>.po a translated catalog.
i18n.gettext(GETTEXT_PACKAGE,
  preset : 'glib',
  args : [
    '--from-code=UTF-8',
    '--keyword=_',
    '--keyword=N_',
    '--keyword=C_:1c,2',
  ],
)
