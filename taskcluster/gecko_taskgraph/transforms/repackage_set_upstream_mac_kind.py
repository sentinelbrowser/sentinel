# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at http://mozilla.org/MPL/2.0/.
"""
Transform mac notarization tasks
"""

from taskgraph.transforms.base import TransformSequence
from taskgraph.util.schema import resolve_keyed_by

transforms = TransformSequence()


@transforms.add
def repackage_set_upstream_mac_kind(config, tasks):
    """
    Notarization only runs on level 3
    If level < 3 then repackage the mac-signing task artifact
    Exception for debug builds, which will use signed build on level 3
    """
    for task in tasks:
        if "macosx64" not in task["primary-dependency"].attributes["build_platform"]:
            task.pop("upstream-mac-kind")
            yield task
            continue
        resolve_keyed_by(
            task,
            "upstream-mac-kind",
            item_name=config.kind,
            **{
                "build-type": task["primary-dependency"].attributes["build_type"],
                "project": config.params.get("project"),
            }
        )
        upstream_mac_kind = task.pop("upstream-mac-kind")

        if task["primary-dependency"].kind != upstream_mac_kind:
            continue
        yield task
