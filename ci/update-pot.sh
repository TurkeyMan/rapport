#!/bin/sh
# Regenerates i18n/rapport.pot, the template translators start from, from
# the rapport domain's strings in the sources. Needs gettext's xgettext.
#   ci/update-pot.sh
# Then, for each translation: msgmerge -U i18n/rapport.<tag>.po i18n/rapport.pot
set -e
cd "$(dirname "$0")/.."
find core backends -name '*.cpp' -o -name '*.h' | sort | xgettext --from-code=UTF-8 \
    --language=C++ --files-from=- --output=i18n/rapport.pot \
    --package-name=rapport --msgid-bugs-address=https://github.com/TurkeyMan/rapport/issues \
    --add-comments=TRANSLATORS: --sort-by-file \
    --keyword= --keyword=tr:2 --keyword=tr_ctx:2c,3 --keyword=tr_n:2,3 \
    --keyword='KEY:1,"The name of a key in shortcuts such as Ctrl+S"'
# Only the rapport domain: the app's domains are extracted by the apps.
