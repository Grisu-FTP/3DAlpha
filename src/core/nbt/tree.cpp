#include "core/nbt/tree.hpp"

#include <cstdio>

namespace mc::nbt {

namespace {

bool copyListElement(Reader& reader, Writer& writer, TagType type, int depth);

bool copyNamed(Reader& reader, Writer& writer, TagType type, std::string_view name, int depth)
{
    if (depth > kMaxDepth) {
        reader.fail();
        return false;
    }

    switch (type) {
        case TagType::End:
            return true;
        case TagType::Byte:
            writer.writeByte(name, reader.byteValue());
            break;
        case TagType::Short:
            writer.writeShort(name, reader.shortValue());
            break;
        case TagType::Int:
            writer.writeInt(name, reader.intValue());
            break;
        case TagType::Long:
            writer.writeLong(name, reader.longValue());
            break;
        case TagType::Float:
            writer.writeFloat(name, reader.floatValue());
            break;
        case TagType::Double:
            writer.writeDouble(name, reader.doubleValue());
            break;
        case TagType::ByteArray:
            writer.writeByteArray(name, reader.byteArray());
            break;
        case TagType::String:
            writer.writeString(name, reader.string());
            break;
        case TagType::IntArray:
        case TagType::LongArray: {
            // Elements stay big-endian in the source buffer, so this splices
            // the payload through rather than rewriting it value by value.
            const usize start = reader.offset();
            if (!reader.skipValue(type)) {
                return false;
            }
            writer.writeRaw(name, type, reader.rangeSince(start));
            break;
        }
        case TagType::List: {
            TagType elem;
            i32 count;
            if (!reader.enterList(&elem, &count)) {
                return false;
            }
            writer.beginList(name, elem);
            for (i32 i = 0; i < count; ++i) {
                if (!copyListElement(reader, writer, elem, depth + 1)) {
                    return false;
                }
            }
            writer.endList();
            break;
        }
        case TagType::Compound: {
            writer.beginCompound(name);
            TagType member;
            std::string_view memberName;
            while (reader.nextField(&member, &memberName)) {
                if (!copyNamed(reader, writer, member, memberName, depth + 1)) {
                    return false;
                }
            }
            writer.endCompound();
            break;
        }
    }

    return reader.ok() && writer.ok();
}

bool copyListElement(Reader& reader, Writer& writer, TagType type, int depth)
{
    if (depth > kMaxDepth) {
        reader.fail();
        return false;
    }

    switch (type) {
        case TagType::End:
            return true;
        case TagType::Byte:
            writer.listByte(reader.byteValue());
            break;
        case TagType::Short:
            writer.listShort(reader.shortValue());
            break;
        case TagType::Int:
            writer.listInt(reader.intValue());
            break;
        case TagType::Long:
            writer.listLong(reader.longValue());
            break;
        case TagType::Float:
            writer.listFloat(reader.floatValue());
            break;
        case TagType::Double:
            writer.listDouble(reader.doubleValue());
            break;
        case TagType::ByteArray:
            writer.listByteArray(reader.byteArray());
            break;
        case TagType::String:
            writer.listString(reader.string());
            break;
        case TagType::IntArray:
        case TagType::LongArray: {
            const usize start = reader.offset();
            if (!reader.skipValue(type)) {
                return false;
            }
            writer.listRaw(reader.rangeSince(start));
            break;
        }
        case TagType::List: {
            TagType elem;
            i32 count;
            if (!reader.enterList(&elem, &count)) {
                return false;
            }
            writer.beginListElementList(elem);
            for (i32 i = 0; i < count; ++i) {
                if (!copyListElement(reader, writer, elem, depth + 1)) {
                    return false;
                }
            }
            writer.endList();
            break;
        }
        case TagType::Compound: {
            writer.beginListElementCompound();
            TagType member;
            std::string_view memberName;
            while (reader.nextField(&member, &memberName)) {
                if (!copyNamed(reader, writer, member, memberName, depth + 1)) {
                    return false;
                }
            }
            writer.endCompound();
            break;
        }
    }

    return reader.ok() && writer.ok();
}

void appendIndent(std::string& out, int depth)
{
    out.append(static_cast<usize>(depth) * 2, ' ');
}

void appendNumber(std::string& out, const char* format, double value)
{
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), format, value);
    out += buffer;
}

void appendInteger(std::string& out, long long value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%lld", value);
    out += buffer;
}

bool dumpValue(Reader& reader, std::string& out, TagType type, int depth);

bool dumpCompoundBody(Reader& reader, std::string& out, int depth)
{
    TagType type;
    std::string_view name;
    while (reader.nextField(&type, &name)) {
        appendIndent(out, depth);
        out += tagTypeName(type);
        out += ' ';
        out.append(name.data(), name.size());
        out += ": ";
        if (!dumpValue(reader, out, type, depth)) {
            return false;
        }
    }
    return reader.ok();
}

bool dumpValue(Reader& reader, std::string& out, TagType type, int depth)
{
    if (depth > kMaxDepth) {
        reader.fail();
        return false;
    }

    switch (type) {
        case TagType::End:
            out += "end\n";
            break;
        case TagType::Byte:
            appendInteger(out, reader.byteValue());
            out += '\n';
            break;
        case TagType::Short:
            appendInteger(out, reader.shortValue());
            out += '\n';
            break;
        case TagType::Int:
            appendInteger(out, reader.intValue());
            out += '\n';
            break;
        case TagType::Long:
            appendInteger(out, static_cast<long long>(reader.longValue()));
            out += '\n';
            break;
        case TagType::Float:
            // %.9g / %.17g are the shortest forms that survive a round trip
            // through the binary representation, so a dump comparison never
            // hides a real difference behind printing precision.
            appendNumber(out, "%.9g", reader.floatValue());
            out += '\n';
            break;
        case TagType::Double:
            appendNumber(out, "%.17g", reader.doubleValue());
            out += '\n';
            break;
        case TagType::ByteArray: {
            const ConstByteSpan bytes = reader.byteArray();
            appendInteger(out, static_cast<long long>(bytes.size()));
            out += " bytes [";
            const usize preview = bytes.size() < 8 ? bytes.size() : 8;
            for (usize i = 0; i < preview; ++i) {
                if (i != 0) {
                    out += ' ';
                }
                char buffer[4];
                std::snprintf(buffer, sizeof(buffer), "%02x", bytes[i]);
                out += buffer;
            }
            out += bytes.size() > preview ? " ...]\n" : "]\n";
            break;
        }
        case TagType::String: {
            const std::string_view value = reader.string();
            out += '"';
            out.append(value.data(), value.size());
            out += "\"\n";
            break;
        }
        case TagType::IntArray:
        case TagType::LongArray: {
            const usize start = reader.offset();
            if (!reader.skipValue(type)) {
                return false;
            }
            appendInteger(out, static_cast<long long>(reader.offset() - start));
            out += " raw bytes\n";
            break;
        }
        case TagType::List: {
            TagType elem;
            i32 count;
            if (!reader.enterList(&elem, &count)) {
                return false;
            }
            appendInteger(out, count);
            out += " x ";
            out += tagTypeName(elem);
            out += '\n';
            for (i32 i = 0; i < count; ++i) {
                appendIndent(out, depth + 1);
                out += "- ";
                if (elem == TagType::Compound) {
                    out += '\n';
                    if (!dumpCompoundBody(reader, out, depth + 2)) {
                        return false;
                    }
                } else if (!dumpValue(reader, out, elem, depth + 1)) {
                    return false;
                }
            }
            break;
        }
        case TagType::Compound:
            out += '\n';
            if (!dumpCompoundBody(reader, out, depth + 1)) {
                return false;
            }
            break;
    }

    return reader.ok();
}

}  // namespace

bool copyValue(Reader& reader, Writer& writer, TagType type, std::string_view name)
{
    return copyNamed(reader, writer, type, name, 0);
}

bool copyDocument(ConstByteSpan in, std::vector<u8>& out)
{
    Reader reader(in);
    std::string_view rootName;
    if (!reader.enterRoot(&rootName)) {
        return false;
    }

    Writer writer(out);
    writer.beginRoot(rootName);

    TagType type;
    std::string_view name;
    while (reader.nextField(&type, &name)) {
        if (!copyNamed(reader, writer, type, name, 0)) {
            return false;
        }
    }
    writer.endRoot();

    return reader.ok() && writer.ok();
}

bool dump(ConstByteSpan in, std::string& out)
{
    Reader reader(in);
    std::string_view rootName;
    if (!reader.enterRoot(&rootName)) {
        return false;
    }

    out += "Compound \"";
    out.append(rootName.data(), rootName.size());
    out += "\"\n";

    return dumpCompoundBody(reader, out, 1);
}

}  // namespace mc::nbt
