#pragma once

#include "matching_engine.hpp"
#include <string>
#include <vector>
#include <sstream>
#include <cctype>
#include <iostream>

namespace matching{

//simple split on ',' (no quotes/escapes handled)
inline std::vector<std::string> splitCSV(const std::string& line){
    std::vector<std::string> fields;
    std::string current;
    for(char c: line){
        if(c == ','){
            fields.push_back(current);
            current.clear();
        }
        else{current.push_back(c);}
    }
    fields.push_back(current);
    return fields;
}

inline std::string trim(const std::string& s){
    std::size_t start = 0;
    while(start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))){++start;}
    std::size_t end = s.size();
    while(end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))){--end;}
    return s.substr(start, end - start);
}

inline bool parseSide(const std::string& token, Side& out){
    if(token == "B"){out = Side::Buy; return true;}
    if(token == "S"){out = Side::Sell; return true;}
    return false;
}

inline bool parseTIF(const std::string& token, TimeInForce& out){
    if(token == "GFD"){out = TimeInForce::GFD; return true;}
    if(token == "IOC"){out = TimeInForce::IOC; return true;}
    if(token == "FOK"){out = TimeInForce::FOK; return true;}
    return false;
}

//parseLine: parse a line into an Event
//returns true on success, false on error (line ignored)
inline bool parseLine(const std::string& rawLine, Event& out){
    std::string line = trim(rawLine);
    if(line.empty()){return false;}
    if(line[0] == '#'){return false;}

    auto fields = splitCSV(line);
    if(fields.empty()){return false;}

    //normalize first field (type)
    std::string type = trim(fields[0]);
    if(type.empty()){return false;}

    char t = type[0];
    if(t == 'L'){
        //Support two formats:
        //L,symbol,side,price,qty,tif
        //L,user,symbol,side,price,qty,tif
        if(fields.size() == 6){
            out.user_id = 1; // default user
            out.symbol  = trim(fields[1]);
            auto sideStr = trim(fields[2]);
            auto pxStr   = trim(fields[3]);
            auto qtyStr  = trim(fields[4]);
            auto tifStr  = trim(fields[5]);

            out.type = EventType::NewLimit;
            if(!parseSide(sideStr, out.side)){
                std::cerr << "Invalid side in line: " << line << "\n";
                return false;
            }
            try{
                out.price = static_cast<Price>(std::stoll(pxStr));
                out.qty = static_cast<Qty>(std::stoll(qtyStr));
            } catch(...){
                std::cerr << "Invalid price/qty in line: " << line << "\n";
                return false;
            }
            if(!parseTIF(tifStr, out.tif)){
                std::cerr << "Invalid TIF in line: " << line << "\n";
                return false;
            }
            out.id = 0;
            return true;

        }
        else if (fields.size() == 7){
            auto userStr = trim(fields[1]);
            auto symbolStr = trim(fields[2]);
            auto sideStr = trim(fields[3]);
            auto pxStr = trim(fields[4]);
            auto qtyStr = trim(fields[5]);
            auto tifStr = trim(fields[6]);

            out.type = EventType::NewLimit;
            try{
                out.user_id = static_cast<UserId>(std::stoll(userStr));
            }catch (...){
                std::cerr << "Invalid user id in line: " << line << "\n";
                return false;
            }
            out.symbol = symbolStr;
            if(!parseSide(sideStr, out.side)){
                std::cerr << "Invalid side in line: " << line << "\n";
                return false;
            }
            try{
                out.price = static_cast<Price>(std::stoll(pxStr));
                out.qty   = static_cast<Qty>(std::stoll(qtyStr));
            }catch(...){
                std::cerr << "Invalid price/qty in line: " << line << "\n";
                return false;
            }
            if(!parseTIF(tifStr, out.tif)){
                std::cerr << "Invalid TIF in line: " << line << "\n";
                return false;
            }
            out.id = 0;
            return true;

        } 
        else{
            std::cerr << "Invalid L line: " << line << "\n";
            return false;
        }
    }
    else if(t == 'M'){
        //M,symbol,side,qty
        //M,user,symbol,side,qty
        if(fields.size() == 4){
            out.user_id = 1; // default
            out.symbol = trim(fields[1]);
            auto sideStr = trim(fields[2]);
            auto qtyStr = trim(fields[3]);

            out.type = EventType::NewMarket;
            if(!parseSide(sideStr, out.side)){
                std::cerr << "Invalid side in line: " << line << "\n";
                return false;
            }
            try{
                out.qty = static_cast<Qty>(std::stoll(qtyStr));
            }catch(...){
                std::cerr << "Invalid qty in line: " << line << "\n";
                return false;
            }
            out.price = 0;
            out.tif = TimeInForce::IOC; //markets never rest
            out.id = 0;
            return true;
        } 
        else if(fields.size() == 5){
            auto userStr = trim(fields[1]);
            auto symbolStr= trim(fields[2]);
            auto sideStr = trim(fields[3]);
            auto qtyStr = trim(fields[4]);

            out.type = EventType::NewMarket;
            try{
                out.user_id = static_cast<UserId>(std::stoll(userStr));
            }catch(...){
                std::cerr << "Invalid user id in line: " << line << "\n";
                return false;
            }
            out.symbol = symbolStr;
            if(!parseSide(sideStr, out.side)){
                std::cerr << "Invalid side in line: " << line << "\n";
                return false;
            }
            try{
                out.qty = static_cast<Qty>(std::stoll(qtyStr));
            }catch(...){
                std::cerr << "Invalid qty in line: " << line << "\n";
                return false;
            }
            out.price = 0;
            out.tif   = TimeInForce::IOC;
            out.id    = 0;
            return true;
        } 
        else{
            std::cerr << "Invalid M line: " << line << "\n";
            return false;
        }
    }
    else if(t == 'C'){
        //C,symbol,orderId
        if(fields.size() != 3){
            std::cerr << "Invalid C line: " << line << "\n";
            return false;
        }
        out.type = EventType::Cancel;
        out.symbol = trim(fields[1]);
        try{
            out.id = static_cast<OrderId>(std::stoll(trim(fields[2])));
        } catch (...){
            std::cerr << "Invalid orderId in line: " << line << "\n";
            return false;
        }
        //other fields unused
        out.side = Side::Buy;
        out.price = 0;
        out.qty = 0;
        out.tif = TimeInForce::GFD;
        return true;
    }
    else if(t == 'R'){
        //R,symbol,oldId,side,price,qty,tif
        if(fields.size() != 7){
            std::cerr << "Invalid R line: " << line << "\n";
            return false;
        }
        out.type = EventType::Replace;
        out.symbol = trim(fields[1]);
        try{
            out.id = static_cast<OrderId>(std::stoll(trim(fields[2]))); //oldId
        } catch (...){
            std::cerr << "Invalid oldId in line: " << line << "\n";
            return false;
        }
        if(!parseSide(trim(fields[3]), out.side)){
            std::cerr << "Invalid side in line: " << line << "\n";
            return false;
        }
        try{
            out.price = static_cast<Price>(std::stoll(trim(fields[4])));
            out.qty = static_cast<Qty>(std::stoll(trim(fields[5])));
        } catch (...){
            std::cerr << "Invalid price/qty in line: " << line << "\n";
            return false;
        }
        if(!parseTIF(trim(fields[6]), out.tif)){
            std::cerr << "Invalid TIF in line: " << line << "\n";
            return false;
        }
        return true;
    } 
    else{
        std::cerr << "Unknown event type in line: " << line << "\n";
        return false;
    }
}
}