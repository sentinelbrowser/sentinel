# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.

import runpy
import string


def generate(output, template, dataFile):
    with open(template, "r") as f:
        template = string.Template(f.read())
    data = runpy.run_path(dataFile)["data"]

    longhand_count = 0
    shorthand_count = 0
    alias_count = 0
    property_ids = []
    for prop in data.values():
        if prop.type() != "alias":
            if prop.type() == "longhand":
                assert shorthand_count == 0
                longhand_count += 1
            else:
                assert alias_count == 0
                shorthand_count += 1
            property_ids.append("eCSSProperty_{}".format(prop.id))
        else:
            alias_count += 1
            property_ids.append("eCSSPropertyAlias_{}".format(prop.alias_id))

    output.write("/* THIS IS AN AUTOGENERATED FILE.  DO NOT EDIT */\n\n")
    output.write(
        template.substitute(
            {
                "property_ids": "\n".join("  {},".format(p) for p in property_ids),
                "longhand_count": property_ids[longhand_count],
                "shorthand_count": property_ids[longhand_count + shorthand_count],
            }
        )
    )
