
#include "Map.h"
#include <ctime>
#include <utility>
#include <vector>

void In_Memory_Storage :: set(std::string key, std::string value, long long expiration_time ) {
        storage[key] = make_pair(value,expiration_time) ;
}

std::string In_Memory_Storage :: get(std::string key,long long expiration_time) {
        clean(expiration_time);
        
       // std :: cout << "current is " << storage.size() << std :: endl;
       // std :: cout << "key is " << key << std :: endl;
        // for(auto it : storage) {
        //     std :: cout << it.first << " " << it.second.first << std :: endl;
        // }
        if(!storage.count(key)){
                return "";
        }
        return storage[key].first;
}

void In_Memory_Storage :: clean(long long current_time ){
    std :: map<std::string , std :: pair<std :: string , long long > > tmp ;
   // std :: cout << current_time << std :: endl;
   // std :: cout << "Size before " << storage.size() << std :: endl; 
    //std :: cout << current_time << std::endl;
    for(auto it : storage){
        //std :: cout << it.second.second << std::endl;
        if(it.second.second  >= current_time){
            tmp[it.first] = it.second;
        }
    }
   storage.clear();
   for(auto it : tmp){
        storage[it.first] = it.second;
   }
   //std :: cout << "Size after " << storage.size() << std :: endl; 
   tmp.clear();
}

std::vector<std::string> In_Memory_Storage :: getAllKey(){
    std::vector<std::string> keys ;
    for(auto it : storage){
        if(it.first != "dir" && it.first != "dbfilename") keys.push_back(it.first);
    }
    return keys;
}


bool In_Memory_Storage :: exist(std::string key){
    return storage.count(key);
}

bool In_Memory_Storage :: exist_type(std:: string key){
    return type.count(key);
}

 void In_Memory_Storage::set_type(std::string key, std::string key_type ){
    type[key] = key_type;
}

std::string In_Memory_Storage::get_type(std::string key ){
    return type[key] ;
}

void In_Memory_Storage :: set_seq(std::string seq_no){
    sequence_no[seq_no] ++;
}

std::vector<std::string> In_Memory_Storage :: get_all_seq(){
    std::vector<std::string > all ;
    for(auto it : sequence_no){
        all.push_back(it.first);
    }
    return all;
}

void In_Memory_Storage ::set_stream(std::pair<std::string, std::string> key, std::pair<std::string, std::string> value){
    stream[key] = value ;
}


std::pair<std::string, std::string> In_Memory_Storage::get_stream(std::pair<std::string, std::string> key){
    return stream[key];
}

std::vector<std::vector<std::string> > In_Memory_Storage :: get_range(std::string left, std::string right){
    std::vector<std::vector<std::string> > v;
    for(auto it : stream ){
        std::string range_key = it.first.second;
        if(range_key.compare(left) >= 0 && right.compare(range_key) >= 0){
            std::vector<std::string> internal ;
            internal.push_back(it.first.second);
            internal.push_back(it.second.first);
            internal.push_back(it.second.second);
            v.push_back(internal);
        }
    }
    return v;
}
