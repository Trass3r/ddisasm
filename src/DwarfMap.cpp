//===- DwarfMap.cpp ---------------------------------------------*- C++ -*-===//
//
//  Copyright (C) 2020 GrammaTech, Inc.
//
//  This code is licensed under the GNU Affero General Public License
//  as published by the Free Software Foundation, either version 3 of
//  the License, or (at your option) any later version. See the
//  LICENSE.txt file in the project root for license terms or visit
//  https://www.gnu.org/licenses/agpl.txt.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
//  GNU Affero General Public License for more details.
//
//  This project is sponsored by the Office of Naval Research, One Liberty
//  Center, 875 N. Randolph Street, Arlington, VA 22203 under contract #
//  N68335-17-C-0700.  The content of the information does not necessarily
//  reflect the position or policy of the Government and no official
//  endorsement should be inferred.
//
//===----------------------------------------------------------------------===//

#include "DwarfMap.h"
#include <fcntl.h>
#include <libdwarf/libdwarf.h>
#include <stdio.h>
#include <unistd.h>

DwarfMap::DwarfMap(const std::string& filename)
{
    this->dwarf_fd = open(filename.c_str(), O_RDONLY);
    if(this->dwarf_fd > 0)
    {
        int res = dwarf_init(this->dwarf_fd, DW_DLC_READ, this->handler, this->error_argument,
                             &this->debug, &this->error);

        if(res != DW_DLV_OK)
        {
            fprintf(stderr, "Unable to open executable");
            dwarf_finish(this->debug, &this->error);
            close(this->dwarf_fd);
            this->dwarf_fd = 0;
        }
    }
}

DwarfMap::~DwarfMap()
{
    if(this->dwarf_fd > 0)
    {
        dwarf_finish(this->debug, &this->error);
        close(this->dwarf_fd);
    }
}

void DwarfMap::traverse_compilation_units()
{
    Dwarf_Unsigned cu_header_len = 0;
    Dwarf_Unsigned abbrev_offset = 0;
    Dwarf_Unsigned next_cu_header_index = 0;
    Dwarf_Half version_stamp = 0;
    Dwarf_Half address_size = 0;
    Dwarf_Error error = 0;
    bool dwarf_cu_next = true;

    while(dwarf_cu_next)
    {
        Dwarf_Die no_die = 0;
        Dwarf_Die cu_die = 0;
        Dwarf_Signed result =
            dwarf_next_cu_header(this->debug, &cu_header_len, &version_stamp, &abbrev_offset,
                                 &address_size, &next_cu_header_index, &error);

        if(result == DW_DLV_ERROR)
        {
            fprintf(stderr, "Error within the compilation unit header");
            dwarf_cu_next = false;
            break;
        }
        if(result == DW_DLV_NO_ENTRY)
        {
            // Completed
            dwarf_cu_next = false;
            break;
        }

        result = dwarf_siblingof(this->debug, no_die, &cu_die, &error);
        if(result == DW_DLV_NO_ENTRY)
        {
            printf("Error in dwarf_siblingof on CU die \n");
            dwarf_cu_next = false;
            break;
        }

        traverse(cu_die, 0);
        dwarf_dealloc(this->debug, cu_die, DW_DLA_DIE);
    }
}

void DwarfMap::traverse(Dwarf_Die die, int level)
{
    int res = DW_DLV_ERROR;
    Dwarf_Die cur_die = die;
    Dwarf_Die sib_die = die;
    Dwarf_Die child = 0;
    Dwarf_Error error;

    retrieve_die_data(die);
    res = dwarf_child(cur_die, &child, &error);

    if(res == DW_DLV_OK)
    {
        traverse(child, level + 1);
        sib_die = child;

        // TODO: Change to queue instead of recursion
        while(res == DW_DLV_OK)
        {
            cur_die = sib_die;
            res = dwarf_siblingof(this->debug, cur_die, &sib_die, &error);
            traverse(sib_die, level + 1); /* recur others */
        }
    }
}

void DwarfMap::retrieve_die_data(Dwarf_Die die)
{
    char* name = 0;
    Dwarf_Error error = 0;
    Dwarf_Half tag = 0;
    const char* tagname = 0;

    Dwarf_Bool bAttr;
    Dwarf_Attribute attr;
    Dwarf_Unsigned in_line;
    Dwarf_Unsigned in_file = 0;

    Dwarf_Locdesc* loc_list;
    Dwarf_Signed num_loc;
    Dwarf_Off ptr_address = 0;

    dwarf_diename(die, &name, &error);
    !dwarf_attr(die, DW_AT_decl_line, &attr, &error) && !dwarf_formudata(attr, &in_line, &error);
    !dwarf_attr(die, DW_AT_decl_file, &attr, &error) && !dwarf_formudata(attr, &in_file, &error);
    int got_loclist = !dwarf_hasattr(die, DW_AT_location, &bAttr, &error)
                      && !dwarf_attr(die, DW_AT_location, &attr, &error)
                      && !dwarf_loclist(attr, &loc_list, &num_loc, &error);
    !dwarf_tag(die, &tag, &error) && dwarf_get_TAG_name(tag, &tagname);

    if(got_loclist && loc_list[0].ld_cents == 1)
    {
        dwarf_formref(attr, &ptr_address, &error);

        if(tag == DW_TAG_variable)
        {
            this->dwarfdata.insert(std::make_pair(
                loc_list[0].ld_s[0].lr_number,
                DwarfData(loc_list[0].ld_s[0].lr_number, name, tag, in_file, in_line)));
        }
    }
    dwarf_dealloc(this->debug, name, DW_DLA_STRING);
}

void DwarfMap::extract_dwarf_data()
{
    traverse_compilation_units();
}

void DwarfMap::flag_constsym(gtirb::Module& module)
{
    std::map<gtirb::UUID, gtirb::UUID> in_debug;
    for(auto i = module.symbols().begin(); i != module.symbols().end(); i++)
    {
        const gtirb::Symbol& symentry = *i;

        if(std::optional<gtirb::Addr> addr = symentry.getAddress(); addr)
        {
            auto ad = static_cast<uint64_t>(*addr);
            auto entryFound = this->dwarfdata.find(ad);
            // If the section is not dwarf, analyse it.
            if(entryFound != this->dwarfdata.end())
            {
                in_debug[symentry.getUUID()] = symentry.getUUID();
            }
        }
    }
    module.addAuxData<gtirb::schema::FlaggedSections>(std::move(in_debug));
}

DwarfData::DwarfData(uint64_t addr, const char* name, Dwarf_Half tag, Dwarf_Unsigned file_no,
                     Dwarf_Unsigned line_no)
{
    this->addr = addr;
    this->name = {name};
    this->tag = tag;
    this->file_no = file_no;
    this->line_no = line_no;
}
