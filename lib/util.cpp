//This line will make ftello/fseeko work with 64 bits numbers
#define _FILE_OFFSET_BITS 64

#include "util.h"
#include "timing.h"
#include "defines.h"
#include "bitfields.h"
#include <stdio.h>
#include <iostream>

#define RECORD_POINTER p+getOffset()+(getRecordPosition(recordNo)*getRSize())+fd.offset
#define RAXHDR_FIELDOFFSET p[1]
#define RAXHDR_RECORDCNT *(uint32_t*)(p+2)
#define RAXHDR_RECORDSIZE *(uint32_t*)(p+6)
#define RAXHDR_STARTPOS *(uint32_t*)(p+10)
#define RAXHDR_DELETED *(uint64_t*)(p+14)
#define RAXHDR_PRESENT *(uint32_t*)(p+22)
#define RAXHDR_OFFSET *(uint16_t*)(p+26)
#define RAX_REQDFIELDS_LEN 28

namespace Util{
  bool stringScan(const std::string & src, const std::string & pattern, std::deque<std::string> & result){
    result.clear();
    std::deque<size_t> positions;
    size_t pos = pattern.find("%", 0);
    while (pos != std::string::npos){
      positions.push_back(pos);
      pos = pattern.find("%", pos + 1);
    }
    if (positions.size() == 0){
      return false;
    }
    size_t sourcePos = 0;
    size_t patternPos = 0;
    std::deque<size_t>::iterator posIter = positions.begin();
    while (sourcePos != std::string::npos){
    //Match first part of the string
      if (pattern.substr(patternPos, *posIter - patternPos) != src.substr(sourcePos, *posIter - patternPos)){
        break;
      }
      sourcePos += *posIter - patternPos;
      std::deque<size_t>::iterator nxtIter = posIter + 1;
      if (nxtIter != positions.end()){
        patternPos = *posIter+2;
        size_t tmpPos = src.find(pattern.substr(*posIter+2, *nxtIter - patternPos), sourcePos);
        result.push_back(src.substr(sourcePos, tmpPos - sourcePos));
        sourcePos = tmpPos;
      }else{
        result.push_back(src.substr(sourcePos));
        sourcePos = std::string::npos;
      }
      posIter++;
    }
    return result.size() == positions.size();
  }

  /// 64-bits version of ftell
  uint64_t ftell(FILE * stream){
    /// \TODO Windows implementation (e.g. _ftelli64 ?)
    return ftello(stream);
  }

  /// 64-bits version of fseek
  uint64_t fseek(FILE * stream, uint64_t offset, int whence){
    /// \TODO Windows implementation (e.g. _fseeki64 ?)
    return fseeko(stream, offset, whence);
  }

  ///If waitReady is true (default), waits for isReady() to return true in 50ms sleep increments.
  RelAccX::RelAccX(char * data, bool waitReady){
    if (!data){
      p = 0;
      return;
    }
    p = data;
    if (waitReady){
      while (!isReady()){Util::sleep(50);}
    }
    if (isReady()){
      uint16_t offset = RAXHDR_FIELDOFFSET;
      if (offset < 11 || offset >= getOffset()){
        FAIL_MSG("Invalid field offset: %u", offset);
        p = 0;
        return;
      }
      uint32_t dataOffset = 0;
      while (offset < getOffset()){
        const uint8_t sizeByte = p[offset];
        const uint8_t nameLen = sizeByte >> 3;
        const uint8_t typeLen = sizeByte & 0x7;
        const uint8_t fieldType = p[offset+1+nameLen];
        const std::string fieldName(p+offset+1, nameLen);
        uint32_t size = 0;
        switch (typeLen){
          case 1://derived from field type
            if ((fieldType & 0xF0) == RAX_UINT || (fieldType & 0xF0) == RAX_INT){
              //Integer types - lower 4 bits +1 are size in bytes
              size = (fieldType & 0x0F) + 1;
            }else{
              if ((fieldType & 0xF0) == RAX_STRING || (fieldType & 0xF0) == RAX_RAW){
                //String types - 8*2^(lower 4 bits) is size in bytes
                size = 16 << (fieldType & 0x0F);
              }else{
                WARN_MSG("Unhandled field type!");
              }
            }
            break;
          //Simple sizes in bytes
          case 2: size = p[offset+1+nameLen+1]; break;
          case 3: size = *(uint16_t*)(p+offset+1+nameLen+1); break;
          case 4:
            size = Bit::btoh24(p+offset+1+nameLen+1);
            break;
          case 5: size = *(uint32_t*)(p+offset+1+nameLen+1); break;
          default:
            WARN_MSG("Unhandled field data size!");
            break;
        }
        fields[fieldName] = RelAccXFieldData(fieldType, size, dataOffset);
        DONTEVEN_MSG("Field %s: type %u, size %lu, offset %lu", fieldName.c_str(), fieldType, size, dataOffset);
        dataOffset += size;
        offset += nameLen + typeLen + 1;
      }
    }
  }

  ///Gets the amount of records present in the structure.
  uint32_t RelAccX::getRCount() const{return RAXHDR_RECORDCNT;}

  ///Gets the size in bytes of a single record in the structure.
  uint32_t RelAccX::getRSize() const{return RAXHDR_RECORDSIZE;}
  
  ///Gets the position in the records where the entries start
  uint32_t RelAccX::getStartPos() const{return RAXHDR_STARTPOS;}

  ///Gets the number of deleted records
  uint64_t RelAccX::getDeleted() const{return RAXHDR_DELETED;}

  ///Gets the number of records present
  ///Defaults to the record count if set to zero.
  uint32_t RelAccX::getPresent() const{return (RAXHDR_PRESENT ? RAXHDR_PRESENT : RAXHDR_RECORDCNT);}

  ///Gets the offset from the structure start where records begin.
  uint16_t RelAccX::getOffset() const{return *(uint16_t*)(p+26);}

  ///Returns true if the structure is ready for read operations.
  bool RelAccX::isReady() const{return p && (p[0] & 1);}

  ///Returns true if the structure will no longer be updated.
  bool RelAccX::isExit() const{return !p || (p[0] & 2);}

  ///Returns true if the structure should be reloaded through out of band means.
  bool RelAccX::isReload() const{return p[0] & 4;}

  ///Returns true if the given record number can be accessed.
  bool RelAccX::isRecordAvailable(uint64_t recordNo) const{
    //Check if the record has been deleted
    if (getDeleted() > recordNo){return false;}
    //Check if the record hasn't been created yet
    if (recordNo - getDeleted() >= getPresent()){return false;}
    return true;
  }

  ///Converts the given record number into an offset of records after getOffset()'s offset.
  ///Does no bounds checking whatsoever, allowing access to not-yet-created or already-deleted records.
  ///This access method is stable with changing start/end positions and present record counts, because it only
  ///depends on the record count, which may not change for ring buffers.
  uint32_t RelAccX::getRecordPosition(uint64_t recordNo) const{
    if (getRCount()){
      return recordNo % getRCount();
    }else{
      return recordNo;
    }
  }

  ///Returns the (max) size of the given field.
  ///For string types, returns the exact size excluding terminating null byte.
  ///For other types, returns the maximum size possible.
  ///Returns 0 if the field does not exist.
  uint32_t RelAccX::getSize(const std::string & name, uint64_t recordNo) const{
    if (!fields.count(name) || !isRecordAvailable(recordNo)){return 0;}
    const RelAccXFieldData & fd = fields.at(name);
    if ((fd.type & 0xF0) == RAX_STRING){
      return strnlen(RECORD_POINTER, fd.size);
    }else{
      return fd.size;
    }
  }

  ///Returns a pointer to the given field in the given record number.
  ///Returns a null pointer if the field does not exist.
  char * RelAccX::getPointer(const std::string & name, uint64_t recordNo) const{
    if (!fields.count(name)){return 0;}
    const RelAccXFieldData & fd = fields.at(name);
    return RECORD_POINTER;
  }

  ///Returns the value of the given integer-type field in the given record, as an uint64_t type.
  ///Returns 0 if the field does not exist or is not an integer type.
  uint64_t RelAccX::getInt(const std::string & name, uint64_t recordNo) const{
    if (!fields.count(name)){return 0;}
    const RelAccXFieldData & fd = fields.at(name);
    char * ptr = RECORD_POINTER;
    if ((fd.type & 0xF0) == RAX_UINT){//unsigned int
      switch (fd.size){
        case 1: return *(uint8_t*)ptr;
        case 2: return *(uint16_t*)ptr;
        case 3: return Bit::btoh24(ptr);
        case 4: return *(uint32_t*)ptr;
        case 8: return *(uint64_t*)ptr;
        default: WARN_MSG("Unimplemented integer");
      }
    }
    if ((fd.type & 0xF0) == RAX_INT){//signed int
      switch (fd.size){
        case 1: return *(int8_t*)ptr;
        case 2: return *(int16_t*)ptr;
        case 3: return Bit::btoh24(ptr);
        case 4: return *(int32_t*)ptr;
        case 8: return *(int64_t*)ptr;
        default: WARN_MSG("Unimplemented integer");
      }
    }
    return 0; //Not an integer type, or not implemented
  }


  std::string RelAccX::toPrettyString() const{
    std::stringstream r;
    uint64_t delled = getDeleted();
    uint64_t max = delled+getRCount();
    r << "RelAccX: " << getRCount() << " x " << getRSize() << "b @" << getOffset() << " (#" << getDeleted() << " - #" << (getDeleted()+getPresent()-1) << ")" << std::endl;
    for (uint64_t i = delled; i < max; ++i){
      r << "  #" << i << ":" << std::endl;
      for (std::map<std::string, RelAccXFieldData>::const_iterator it = fields.begin(); it != fields.end(); ++it){
        r << "    " << it->first << ": ";
        switch (it->second.type & 0xF0){
          case RAX_INT: r << (int64_t)getInt(it->first, i) << std::endl; break;
          case RAX_UINT: r << getInt(it->first, i) << std::endl; break;
          case RAX_STRING: r << getPointer(it->first, i) << std::endl; break;
          default: r << "[UNIMPLEMENTED]" << std::endl; break;
        }
      }
    }
    return r.str();
  }

  /// Returns the default size in bytes of the data component of a field type number.
  /// Returns zero if not implemented, unknown or the type has no default.
  uint32_t RelAccX::getDefaultSize(uint8_t fType){
    if ((fType & 0XF0) == RAX_INT || (fType & 0XF0) == RAX_UINT){
      return (fType & 0x0F) + 1;//Default size is lower 4 bits plus one bytes
    }
    if ((fType & 0XF0) == RAX_STRING || (fType & 0XF0) == RAX_RAW){
      return 16 << (fType & 0x0F);//Default size is 16 << (lower 4 bits) bytes
    }
    return 0;
  }

  /// Adds a new field to the internal list of fields.
  /// Can only be called if not ready, exit or reload.
  /// Changes the offset and record size to match.
  /// Fails if called multiple times with the same field name.
  void RelAccX::addField(const std::string & name, uint8_t fType, uint32_t fLen){
    if (isExit() || isReload() || isReady()){
      WARN_MSG("Attempting to add a field to a non-writeable memory area");
      return;
    }
    if (!name.size() || name.size() > 31){
      WARN_MSG("Attempting to add a field with illegal name: %s (%u chars)", name.c_str(), name.size());
      return;
    }
    //calculate fLen if missing
    if (!fLen){
      fLen = getDefaultSize(fType);
      if (!fLen){
        WARN_MSG("Attempting to add a mandatory-size field without size");
        return;
      }
    }
    //We now know for sure fLen is set
    //Get current offset and record size
    uint16_t & offset = RAXHDR_OFFSET;
    uint32_t & recSize = RAXHDR_RECORDSIZE;
    //The first field initializes the offset and record size.
    if (!fields.size()){
      recSize = 0;//Nothing yet, this is the first data field.
      offset = RAX_REQDFIELDS_LEN;//All mandatory fields are first - so we start there.
      RAXHDR_FIELDOFFSET = offset;//store the field_offset
    }
    uint8_t typeLen = 1;
    //Check if fLen is a non-default value
    if (getDefaultSize(fType) != fLen){
      //Calculate the smallest size integer we can fit this in
      typeLen = 5;//32 bit
      if (fLen < 0x10000){typeLen = 3;}//16 bit
      if (fLen < 0x100){typeLen = 2;}//8 bit
    }
    //store the details for internal use
    //recSize is the field offset, since we haven't updated it yet
    fields[name] = RelAccXFieldData(fType, fLen, recSize);

    //write the data to memory
    p[offset] = (name.size() << 3) | (typeLen & 0x7);
    memcpy(p+offset+1, name.data(), name.size());
    p[offset+1+name.size()] = fType;
    if (typeLen == 2){*(uint8_t*)(p+offset+2+name.size()) = fLen;}
    if (typeLen == 3){*(uint16_t*)(p+offset+2+name.size()) = fLen;}
    if (typeLen == 5){*(uint32_t*)(p+offset+2+name.size()) = fLen;}

    //Calculate new offset and record size
    offset += 1+name.size()+typeLen;
    recSize += fLen;
  }

  ///Sets the record counter to the given value.
  void RelAccX::setRCount(uint32_t count){RAXHDR_RECORDCNT = count;}

  ///Sets the position in the records where the entries start
  void RelAccX::setStartPos(uint32_t n){RAXHDR_STARTPOS = n;}

  ///Sets the number of deleted records
  void RelAccX::setDeleted(uint64_t n){RAXHDR_DELETED = n;}
  
  ///Sets the number of records present
  ///Defaults to the record count if set to zero.
  void RelAccX::setPresent(uint32_t n){RAXHDR_PRESENT = n;}


  ///Sets the ready flag.
  ///After calling this function, addField() may no longer be called.
  ///Fails if exit, reload or ready are (already) set.
  void RelAccX::setReady(){
    if (isExit() || isReload() || isReady()){
      WARN_MSG("Could not set ready on structure with pre-existing state");
      return;
    }
    p[0] |= 1;
  }

  //Sets the exit flag.
  ///After calling this function, addField() may no longer be called.
  void RelAccX::setExit(){p[0] |= 2;}

  //Sets the reload flag.
  ///After calling this function, addField() may no longer be called.
  void RelAccX::setReload(){p[0] |= 4;}

  ///Writes the given string to the given field in the given record.
  ///Fails if ready is not set.
  ///Ensures the last byte is always a zero.
  void RelAccX::setString(const std::string & name, const std::string & val, uint64_t recordNo){
    if (!fields.count(name)){
      WARN_MSG("Setting non-existent string %s", name.c_str());
      return;
    }
    const RelAccXFieldData & fd = fields.at(name);
    if ((fd.type & 0xF0) != RAX_STRING){
      WARN_MSG("Setting non-string %s", name.c_str());
      return;
    }
    char * ptr = RECORD_POINTER;
    memcpy(ptr, val.data(), std::min((uint32_t)val.size(), fd.size));
    ptr[std::min((uint32_t)val.size(), fd.size-1)] = 0;
  }

  ///Writes the given int to the given field in the given record.
  ///Fails if ready is not set or the field is not an integer type.
  void RelAccX::setInt(const std::string & name, uint64_t val, uint64_t recordNo){
    if (!fields.count(name)){
      WARN_MSG("Setting non-existent integer %s", name.c_str());
      return;
    }
    const RelAccXFieldData & fd = fields.at(name);
    char * ptr = RECORD_POINTER;
    if ((fd.type & 0xF0) == RAX_UINT){//unsigned int
      switch (fd.size){
        case 1: *(uint8_t*)ptr = val; return;
        case 2: *(uint16_t*)ptr = val; return;
        case 3: Bit::htob24(ptr, val); return;
        case 4: *(uint32_t*)ptr = val; return;
        case 8: *(uint64_t*)ptr = val; return;
        default: WARN_MSG("Unimplemented integer size %u", fd.size); return;
      }
    }
    if ((fd.type & 0xF0) == RAX_INT){//signed int
      switch (fd.size){
        case 1: *(int8_t*)ptr = (int64_t)val; return;
        case 2: *(int16_t*)ptr = (int64_t)val; return;
        case 3: Bit::htob24(ptr, val); return;
        case 4: *(int32_t*)ptr = (int64_t)val; return;
        case 8: *(int64_t*)ptr = (int64_t)val; return;
        default: WARN_MSG("Unimplemented integer size %u", fd.size); return;
      }
    }
    WARN_MSG("Setting non-integer %s", name.c_str());
  }

  ///Updates the deleted record counter, the start position and the present record counter, shifting the ring buffer start position forward without moving the ring buffer end position.
  ///If the records present counter would be pushed into the negative by this function, sets it to zero, defaulting it to the record count for all relevant purposes.
  void RelAccX::deleteRecords(uint32_t amount){
    uint32_t & startPos = RAXHDR_STARTPOS;
    uint64_t & deletedRecs = RAXHDR_DELETED;
    uint32_t & recsPresent = RAXHDR_PRESENT;
    startPos += amount;//update start position
    deletedRecs += amount;//update deleted record counter
    if (recsPresent >= amount){
      recsPresent -= amount;//decrease records present
    }else{
      recsPresent = 0;
    }
  }

  ///Updates the present record counter, shifting the ring buffer end position forward without moving the ring buffer start position.
  ///If the records present counter would be pushed past the record counter by this function, sets it to zero, defaulting it to the record count for all relevant purposes.
  void RelAccX::addRecords(uint32_t amount){
    uint32_t & recsPresent = RAXHDR_PRESENT;
    uint32_t & recordsCount = RAXHDR_RECORDCNT;
    if (recsPresent+amount > recordsCount){
      recsPresent = 0;
    }else{
      recsPresent += amount;
    }
  }

}

