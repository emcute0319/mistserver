/// \file dtscmerge.cpp
/// Contains the code that will attempt to merge multiple files into a single DTSC file.

#include <string>
#include <vector>
#include <mist/config.h>
#include <mist/dtsc.h>

namespace Converters {
  int getNextFree( std::map<std::string,std::map<int,int> > mapping ){
    int result = 1;
    std::map<std::string,std::map<int,int> >::iterator mapIt;
    std::map<int,int>::iterator subIt;
    if (mapping.size()){
      for (mapIt = mapping.begin(); mapIt != mapping.end(); mapIt++){
        if (mapIt->second.size()){
          for (subIt = mapIt->second.begin(); subIt != mapIt->second.end(); subIt++){
            if (subIt->second >= result){
              result = subIt->second + 1;
            }
          }
        }
      }
    }
    return result;
  }

  struct keyframeInfo{
    std::string fileName;
    int trackID;
    int keyTime;
    int keyBPos;
    int keyNum;
    int keyLen;
    int endBPos;
  };//keyframeInfo struct

  int DTSCMerge(int argc, char ** argv){
    Util::Config conf = Util::Config(argv[0], PACKAGE_VERSION);
    conf.addOption("output", JSON::fromString("{\"arg_num\":1, \"arg\":\"string\", \"help\":\"Filename of the output file.\"}"));
    conf.addOption("input", JSON::fromString("{\"arg_num\":2, \"arg\":\"string\", \"help\":\"Filename of the first input file.\"}"));
    conf.addOption("[additional_inputs ...]", JSON::fromString("{\"arg_num\":3, \"default\":\"\", \"arg\":\"string\", \"help\":\"Filenames of any number of aditional inputs.\"}"));
    conf.parseArgs(argc, argv);

    DTSC::File outFile;
    JSON::Value meta;
    JSON::Value newMeta;
    std::map<std::string,std::map<int, int> > trackMapping;

    bool fullSort = true;
    std::map<std::string, DTSC::File> inFiles;
    std::string outFileName = argv[1];
    std::string tmpFileName;
    for (int i = 2; i < argc; i++){
      tmpFileName = argv[i];
      if (tmpFileName == outFileName){
        fullSort = false;
      }else{
        inFiles.insert(std::pair<std::string,DTSC::File>(tmpFileName,DTSC::File(tmpFileName)));
      }
    }

    if (fullSort){
      outFile = DTSC::File(outFileName, true);
    }else{
      outFile = DTSC::File(outFileName);
      meta = outFile.getMeta();
      newMeta = meta;
      if (meta.isMember("tracks") && meta["tracks"].size() > 0){
        for (JSON::ObjIter trackIt = meta["tracks"].ObjBegin(); trackIt != meta["tracks"].ObjEnd(); trackIt++){
          trackMapping[argv[1]].insert(std::pair<int,int>(trackIt->second["trackid"].asInt(),getNextFree(trackMapping)));
          newMeta["tracks"][trackIt->first]["trackid"] = trackMapping[argv[1]][trackIt->second["trackid"].asInt()];
        }
      }
    }

    std::multimap<int,keyframeInfo> allSorted;

    for (std::map<std::string,DTSC::File>::iterator it = inFiles.begin(); it != inFiles.end(); it++){
      JSON::Value tmpMeta = it->second.getMeta();
      if (tmpMeta.isMember("tracks") && tmpMeta["tracks"].size() > 0){
        for (JSON::ObjIter trackIt = tmpMeta["tracks"].ObjBegin(); trackIt != tmpMeta["tracks"].ObjEnd(); trackIt++){
          long long int oldID = trackIt->second["trackid"].asInt();
          long long int mappedID = getNextFree(trackMapping);
          trackMapping[it->first].insert(std::pair<int,int>(oldID,mappedID));
          trackIt->second["trackid"] = mappedID;
          for (int i = 0; i < trackIt->second["keytime"].size(); i++){
            ///\todo Update to new struct.
            keyframeInfo tmpInfo;
            tmpInfo.fileName = it->first;
            tmpInfo.trackID = oldID;
            tmpInfo.keyTime = trackIt->second["keytime"][i].asInt();
            tmpInfo.keyBPos = trackIt->second["keybpos"][i].asInt();
            tmpInfo.keyNum = trackIt->second["keynum"][i].asInt();
            tmpInfo.keyLen = trackIt->second["keylen"][i].asInt();
            if ( i < trackIt->second["keytime"].size() - 1 ){
              tmpInfo.endBPos = trackIt->second["keybpos"][i + 1].asInt();
            }else{
              tmpInfo.endBPos = it->second.getBytePosEOF();
            }
            allSorted.insert(std::pair<int,keyframeInfo>(trackIt->second["keytime"][i].asInt(),tmpInfo));
          }
          trackIt->second.removeMember("keytime");
          trackIt->second.removeMember("keybpos");
          trackIt->second.removeMember("keynum");
          trackIt->second.removeMember("keylen");
          trackIt->second.removeMember("frags");
          newMeta["tracks"][JSON::Value(mappedID).asString()] = trackIt->second;
        }
      }
    }

    if (fullSort){
      meta.null();
      meta["moreheader"] = 0ll;
      std::string tmpWrite = meta.toPacked();
      outFile.writeHeader(tmpWrite,true);
    }

    for (std::multimap<int,keyframeInfo>::iterator sortIt = allSorted.begin(); sortIt != allSorted.end(); sortIt++){
      inFiles[sortIt->second.fileName].seek_bpos(sortIt->second.keyBPos);
      while (inFiles[sortIt->second.fileName].getBytePos() < sortIt->second.endBPos){
        JSON::Value translate = inFiles[sortIt->second.fileName].getJSON();
        if (translate["trackid"].asInt() == sortIt->second.trackID){ 
          translate["trackid"] = trackMapping[sortIt->second.fileName][translate["trackid"].asInt()];
          outFile.writePacket(translate);
        }
        inFiles[sortIt->second.fileName].seekNext();
      }
    }

    fprintf(stderr, "%s\n", newMeta.toPrettyString().c_str());    
    fprintf(stderr, "Oldheader (%d):\n%s\n", meta.toPacked().size(), meta.toPrettyString().c_str());
    std::string writeMeta = newMeta.toPacked();
    meta["moreheader"] = outFile.addHeader(writeMeta);
    fprintf(stderr, "Newheader (%d):\n%s\n", meta.toPacked().size(), meta.toPrettyString().c_str());
    writeMeta = meta.toPacked();
    outFile.writeHeader(writeMeta);

    return 0;
  }
}

int main(int argc, char ** argv){
  return Converters::DTSCMerge(argc, argv);
}