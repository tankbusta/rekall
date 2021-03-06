# Rekall Memory Forensics
#
# Copyright 2015 Google Inc. All Rights Reserved.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or (at
# your option) any later version.
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
#

from rekall import plugin


class VADMapMixin(plugin.VerbosityMixIn):
    """A plugin to display information about virtual address pages."""

    name = "vadmap"

    @classmethod
    def args(cls, parser):
        super(VADMapMixin, cls).args(parser)
        parser.add_argument(
            "--start", default=0, type="IntParser",
            help="Start reading from this page.")

        parser.add_argument(
            "--end", default=2**63, type="IntParser",
            help="Stop reading at this offset.")

    def __init__(self, *args, **kwargs):
        self.start = kwargs.pop("start", 0)
        self.end = kwargs.pop("end", 2**64)
        super(VADMapMixin, self).__init__(*args, **kwargs)

    def FormatMetadata(self, type, metadata, offset=None):
        result = ""
        if "filename" in metadata:
            result += "%s " % metadata["filename"]

        if "number" in metadata:
            result = "PF %s " % metadata["number"]

        if type == "Valid" or type == "Transition":
            result += "PhysAS "

        if offset:
            result += "@ %#x " % offset

        if "ProtoType" in metadata:
            result += "(P) "

        return result

    def GeneratePageMetatadata(self, task):
        """A Generator of vaddr, metadata for each page."""
        _ = task
        return []

    def render(self, renderer):
        for task in self.filter_processes():
            renderer.section()
            renderer.format("Pid: {0} {1}\n", task.pid, task.name)

            headers = [
                ('Virt Addr', 'virt_addr', '[addrpad]'),
                ('Offset', 'offset', '[addrpad]'),
                ('Length', 'length', '[addr]'),
                ('Type', 'type', '20s'),
                ('Comments', 'comments', "")]

            if self.verbosity < 5:
                headers.pop(1)

            renderer.table_header(headers)

            with self.session.plugins.cc() as cc:
                cc.SwitchProcessContext(task)

                old_offset = 0
                old_vaddr = 0
                length = 0x1000
                old_metadata = {}
                for vaddr, metadata in self.GeneratePageMetatadata(task):
                    # Remove the offset so we can merge on identical
                    # metadata (offset will change for each page).
                    offset = metadata.pop("offset", None)

                    # Coalesce similar rows.
                    if ((offset is None or old_offset is None or
                         self.verbosity < 5 or
                         offset == old_offset + length) and
                            metadata == old_metadata and
                            vaddr == old_vaddr + length):
                        length += 0x1000
                        continue

                    type = old_metadata.pop("type", None)
                    if type:
                        comment = self.FormatMetadata(
                            type, old_metadata, offset=old_offset)
                        row = [old_vaddr, old_offset, length, type, comment]
                        if self.verbosity < 5:
                            row.pop(1)

                        renderer.table_row(*row)

                    old_metadata = metadata
                    old_vaddr = vaddr
                    old_offset = offset
                    length = 0x1000
