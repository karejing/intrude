//
//  Created by oldman on 6/8/15.
//

#ifndef __insert_dylib_to_macho__runner__
#define __insert_dylib_to_macho__runner__

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <set>
#include <tuple>
#include "loader.h"
#include "util.h"
#include "exception.h"

static const std::string kExecutablePathWithSlashPrefix = "@executable_path/";
static const std::string kDylibExtensionWithDot = ".dylib";

template <typename MachoBriefInfo>
class Workflow {
public:
    Workflow(const std::string& macho_path) : macho_path_(macho_path) {}
    
    ~Workflow() {};
    
    void run() {
        file_ = fopen(macho_path_.c_str(), "rb+");
        if (file_ != NULL) {
            // 读取所需的Macho结构
            MachoBriefInfo info = do_read_macho_brief_info();
            // 计算要插入的dylib名字
            dylib_name_ = calc_fresh_dylib_name_by_macho_info(info);
            // 根据dylib名字构建要插入的command
            DylibCommandToWrite dylib_command = make_dylib_command_by_name(dylib_name_);
            // 将command写入文件
            insert_command_to_dest_file(dylib_command, info);
            // 修改MachHeder
            MachHeader mach_header = get_modified_header(info, dylib_command);
            // 写入MachHeader
            write_mach_header_to_dest_file(mach_header);
            // 结束
            fclose(file_);
            return;
        }
        throw Exception(ExceptionCode::kShouldNotOccur, util::build_string([&](std::ostringstream& ss) {
            ss << "open file failed, file: " << macho_path_ << ", errno: " << errno;
        }));
    }
    
    std::string fresh_dylib_name() const {
        return dylib_name_;
    }

private:
#pragma mark - types
    typedef typename MachoBriefInfo::MachHeader MachHeader;
    typedef typename MachoBriefInfo::SegmentCommand SegmentCommand;
    typedef typename MachoBriefInfo::DylibCommand DylibCommand;
    typedef typename MachoBriefInfo::LoadCommand LoadCommand;
    typedef typename MachoBriefInfo::SectionHeader SectionHeader;
    static const uint32_t kAlignBase = MachoBriefInfo::kAlignBase;
    
    struct DylibCommandToWrite : public DylibCommand {
        std::string path;
    };
  

#pragma mark - actions
    MachoBriefInfo do_read_macho_brief_info() {
        MachoBriefInfo info;
        
        // read mach header
        info.mach_header = read_record<MachHeader>(0);
        uint32_t offset = sizeof(MachHeader);
        
        // read commands and sections
        for (uint32_t index = 0; index < info.mach_header.value.ncmds; ++index) {
            auto command = read_record<LoadCommand>(offset);
            info.commands.push_back(command);
            if (command.value.cmd == LC_SEGMENT || command.value.cmd == LC_SEGMENT_64) {
                auto segment_command = read_record<SegmentCommand>(offset);
                uint32_t section_header_offset = offset + sizeof(SegmentCommand);
                for (uint32_t section_index = 0; section_index < segment_command.value.nsects; ++section_index) {
                    auto section_header = read_record<SectionHeader>(section_header_offset);
                    info.section_headers.push_back(section_header);
                    section_header_offset += sizeof(SectionHeader);
                }
            }
            offset += command.value.cmdsize;
        }
        
        // save
        return info;
    }
    
    std::string calc_fresh_dylib_name_by_macho_info(const MachoBriefInfo& macho_info) {
        std::set<std::string> names;
        for (auto command : macho_info.commands) {
            if (command.value.cmd == LC_LOAD_DYLIB || command.value.cmd == LC_LOAD_WEAK_DYLIB || command.value.cmd == LC_REEXPORT_DYLIB) {
                auto dylib_command = read_record<DylibCommand>(command.offset);
                auto name_offset = dylib_command.value.dylib.name.offset;
                std::string name = read_cstring(command.offset + name_offset, dylib_command.value.cmdsize - name_offset);
                if (util::string_start_with(name, kExecutablePathWithSlashPrefix)) {
                    names.insert(name.substr(kExecutablePathWithSlashPrefix.length()));
                }
            }
        }
        uint32_t try_max_count = (uint32_t)names.size() + 1;
        for (uint32_t try_index = 0; try_index < try_max_count; ++try_index) {
            std::string name;
            const uint32_t kScale = 26;
            for (uint32_t index = try_index; index != 0; index /= kScale) {
                name.push_back('a' + (index % kScale));
            }
            if (name.size() == 0) {
                name.push_back('a');
            }
            name += kDylibExtensionWithDot;
            if (names.find(name) == names.end()) {
                return name;    // no conflict
            }
        }
        throw Exception(ExceptionCode::kShouldNotOccur, "can not find a suitable name for the dylib");
        return std::string();
    }
    
    DylibCommandToWrite make_dylib_command_by_name(const std::string& dylib_name) {
        DylibCommandToWrite result;
        result.cmd = LC_LOAD_DYLIB;
        result.dylib.timestamp = 2;
        result.dylib.current_version = 0;
        result.dylib.compatibility_version = 0;
        
        // path and length
        result.path = kExecutablePathWithSlashPrefix + dylib_name;
        result.path.push_back('\0');
        
        // align to 32bit or 64bit
        uint32_t length = ((uint32_t)result.path.length() + kAlignBase - 1) / kAlignBase * kAlignBase;
        result.dylib.name.offset = 24; // 4 * 6
        result.cmdsize = result.dylib.name.offset + length;
        
        return result;
    }
    
    MachHeader get_modified_header(const MachoBriefInfo& macho_info, const DylibCommandToWrite& dylib_command) {
        MachHeader result = macho_info.mach_header.value;
        result.ncmds += 1;
        result.sizeofcmds += dylib_command.cmdsize;
        return result;
    }
    
    void insert_command_to_dest_file(const DylibCommandToWrite& new_command, const MachoBriefInfo& src_macho_info) {
        // find position to insert
        // find last command end position
        uint32_t insert_position = 0;
        uint32_t last_command_end_position = 0;
        for (auto command : src_macho_info.commands) {
            uint32_t command_end_position = command.offset + command.value.cmdsize;
            if (command.value.cmd == LC_LOAD_DYLIB || command.value.cmd == LC_LOAD_WEAK_DYLIB) {
                insert_position = std::max(insert_position, command_end_position);
            }
            last_command_end_position = std::max(last_command_end_position, command_end_position);

        }
        
        // find section 1 position
        uint32_t section_1_postion = UINT32_MAX;
        for (auto section_header : src_macho_info.section_headers) {
            if (section_header.value.offset != 0) {
                section_1_postion = section_header.value.offset;
                break;
            }
        }
        
        // check the space for new command
        if (section_1_postion - last_command_end_position < new_command.cmdsize) {
            throw Exception(ExceptionCode::kNotImplement, "now we counldn't move the sections");
        }
        
        // write
        std::vector<uint8_t> buffer(section_1_postion - insert_position);
        util::read(file_, insert_position, section_1_postion - insert_position - new_command.cmdsize, &buffer[0] + new_command.cmdsize);
        std::memcpy(&buffer[0], &new_command, sizeof(DylibCommand));
        std::memcpy(&buffer[0] + sizeof(DylibCommand), &new_command.path[0], new_command.path.length());
        util::write(file_, insert_position, (uint32_t)buffer.size(), &buffer[0]);
    }
    
    void write_mach_header_to_dest_file(const MachHeader& mach_header) {
        util::write(file_, 0, sizeof(MachHeader), (void*)&mach_header);
    }
    
#pragma mark - util    
    template <typename T> T read(uint32_t from) {
        T result;
        util::read(file_, from, sizeof(T), &result);
        return result;
    }
    
    template <typename T> Record<T> read_record(uint32_t from) {
        T result = read<T>(from);
        return Record<T>(from, std::move(result));
    }
    
    std::string read_cstring(uint32_t from, uint32_t max_size) {
        std::string buffer(max_size, 0);
        util::read(file_, from, max_size, &buffer[0]);
        return std::string(&buffer[0]);
    }
    
    std::string dylib_name_;
    std::string macho_path_;
    FILE* file_;
};

#endif /* defined(__insert_dylib_to_macho__runner__) */
