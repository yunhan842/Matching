#pragma once

#include <cstdint>
#include <list>
#include <unordered_map>
#include <functional>
#include <optional>
#include <string>
#include <limits>
#include <algorithm>
#include <iostream>
#include <boost/container/flat_map.hpp>
#include <boost/pool/pool_alloc.hpp>
#include <boost/unordered/unordered_flat_map.hpp>

namespace matching{

using Price = std::int64_t;
using Qty = std::int64_t;
using OrderId = std::int64_t;
using UserId = std::int64_t;

enum class Side {Buy, Sell};
enum class OrderType {Limit, Market};

enum class TimeInForce{GFD, IOC, FOK};

struct Order{
    OrderId id;
    Side side;
    OrderType type;
    Price price;
    Qty qty;
    TimeInForce tif;
};

struct Trade{
    std::string symbol;
    Price price;
    Qty qty;
    OrderId buy_id;
    OrderId sell_id;
};

struct BookStats{
    std::uint64_t trade_count{0};
    Qty traded_qty{0};
    Price last_trade_price{0};
    bool has_last_trade{false};
};

class OrderBook{
public:
    using TradeCallBack = std::function<void(const Trade&)>;

    explicit OrderBook(std::string symbol, TradeCallBack cb = nullptr):
        symbol_(std::move(symbol)), callback_(std::move(cb)), next_id_(1) {}
    
    OrderId addLimit(Side side, Price price, Qty qty, TimeInForce tif = TimeInForce::GFD){
        Order o{next_id_++, side, OrderType::Limit, price, qty, tif};
        //fok: only proceed if we can fully fill immediately
        if(o.tif == TimeInForce::FOK){
            //reject: do nothing
            if(!canFullyMatch(o.side, o.price, o.qty)){return o.id; /*id reserved but no order placed*/}
        }
        match(o);
        //only rest if there's qty left and tif is gfd
        if(o.qty > 0 && o.tif == TimeInForce::GFD){addRestingOrder(o);}
        //if tif is ioc, just drop any unfilled remainder
        return o.id;
    }

    OrderId addMarket(Side side, Qty qty){
        Price px = (side == Side::Buy) ? std::numeric_limits<Price>::max(): std::numeric_limits<Price>::min();
        //for markets, tif doesn’t really matter (they never rest), but must still set *something*:
        Order o{next_id_++, side, OrderType::Market, px, qty, TimeInForce::IOC};
        match(o);
        return o.id;
    }

    bool cancel(OrderId id){
        auto it = index_.find(id);
        if(it == index_.end()){return false;}

        const OrderLocator loc = it->second;
        BookSide& book = (loc.side == Side::Buy) ? bids_: asks_;
        auto lvlIt = book.find(loc.price);
        if(lvlIt == book.end()){
            index_.erase(it);
            return false;
        }
        PriceLevel& lvl = lvlIt->second;
        Qty q = loc.it->qty;
        if(q > 0){lvl.total_qty -= q;}
        lvl.orders.erase(loc.it);
        index_.erase(it);
        if(lvl.orders.empty()){book.erase(lvlIt);}
        return true;
    }

    std::optional<Price> bestBid() const{
        if(bids_.empty()){return std::nullopt;}
        return bids_.rbegin()->first;
    }

    std::optional<Price> bestAsk() const{
        if(asks_.empty()){return std::nullopt;}
        return asks_.begin()->first;
    }

    std::optional<Qty> bestBidSize() const{
        if(bids_.empty()){return std::nullopt;}
        const auto& lvl = std::prev(bids_.end())->second;
        return lvl.total_qty;
    }

    std::optional<Qty> bestAskSize() const{
        if(asks_.empty()){return std::nullopt;}
        const auto& lvl = asks_.begin()->second;
        return lvl.total_qty;
    } 

    std::optional<Price> midPrice() const{
        auto bb = bestBid();
        auto ba = bestAsk();
        if(!bb || !ba){return std::nullopt;}
        return (*bb + *ba) / 2;
    }

    const std::string& symbol() const {return symbol_;}

    void printBook(std::ostream& os, int depth) const {
        os << "OrderBook(" << symbol_ << ")\n";

        //print asks (best first)
        os << "\tAsks:\n";
        int shown = 0;
        for(auto it = asks_.begin(); it != asks_.end() && shown < depth; ++it, ++shown){
            Price px = it->first;
            const PriceLevel& lvl = it->second;
            os << "\t\tpx=" << px << " total_qty=" << lvl.total_qty << " (orders=" << lvl.orders.size() << ")\n";
        }
        if(shown == 0){os << "\t\t<empty>\n";}

        //print bids (best first)
        os << "\tBids:\n";
        shown = 0;
        for(auto it = bids_.rbegin(); it != bids_.rend() && shown < depth; ++it, ++shown){
            Price px = it->first;
            const PriceLevel& lvl = it->second;
            os << "\t\tpx=" << px << " total_qty=" << lvl.total_qty << " (orders=" << lvl.orders.size() << ")\n";}
        if(shown == 0){os << "\t\t<empty>\n";}
    }

    const BookStats& stats() const{return stats_;}

private:
    using OrderList = std::list<Order, boost::fast_pool_allocator<Order>>;

    struct PriceLevel{
        Qty total_qty{0};
        OrderList orders;
    };

    using BookSide = boost::container::flat_map<Price, PriceLevel>;

    struct OrderLocator{
        Side side;
        Price price;
        OrderList::iterator it;
    };

    using OrderIndex = boost::unordered_flat_map<OrderId, OrderLocator>; 
    std::string symbol_;
    TradeCallBack callback_;
    OrderId next_id_;

    BookSide bids_; //key: price; for best bid use rbegin()
    BookSide asks_; //key: price; for best ask use begin()
    OrderIndex index_;
    BookStats stats_;

    void emitTrade(Price price, Qty qty, OrderId buy_id, OrderId sell_id){
        //update stats
        ++stats_.trade_count;
        stats_.traded_qty += qty;
        stats_.last_trade_price = price;
        stats_.has_last_trade = true;
        //notify callback
        if(!callback_){return;}
        Trade t{symbol_, price, qty, buy_id, sell_id};
        callback_(t);
    }

    void match(Order& incoming){
        if(incoming.side == Side::Buy){matchBuy(incoming);}
        else{matchSell(incoming);}
    }

    //match incoming buy against resting asks
    void matchBuy(Order& buy){
        while(buy.qty > 0 && !asks_.empty()){
            auto bestAskIt = asks_.begin();
            Price bestAskPx = bestAskIt->first;
            if(buy.type == OrderType::Limit && buy.price < bestAskPx){break; /*cant cross best price*/}

            PriceLevel& lvl = bestAskIt->second;
            auto it = lvl.orders.begin();
            while(it != lvl.orders.end() && buy.qty > 0){
                Order& sell = *it;
                Qty traded = std::min(buy.qty, sell.qty);
                buy.qty -= traded;
                sell.qty -= traded;
                lvl.total_qty -= traded;

                emitTrade(bestAskPx, traded, buy.id, sell.id);

                if(sell.qty == 0){
                    index_.erase(sell.id);
                    it = lvl.orders.erase(it);
                }
                else{++it;}
            }
            if(lvl.orders.empty()){asks_.erase(bestAskIt);}
        }
    }

    //match incoming sell against resting bids
    void matchSell(Order& sell){
        while(sell.qty > 0 && !bids_.empty()){
            auto bestBidIt = std::prev(bids_.end());
            Price bestBidPx = bestBidIt->first;
            if(sell.type == OrderType::Limit && sell.price > bestBidPx){break;}

            PriceLevel& lvl = bestBidIt->second;
            auto it = lvl.orders.begin();
            while(it != lvl.orders.end() && sell.qty > 0){
                Order& buy = *it;
                Qty traded = std::min(sell.qty, buy.qty);
                sell.qty -= traded;
                buy.qty -= traded;
                lvl.total_qty -= traded;

                emitTrade(bestBidPx, traded, buy.id, sell.id);

                if(buy.qty == 0){
                    index_.erase(buy.id);
                    it = lvl.orders.erase(it);
                }
                else{++it;}
            }
            if(lvl.orders.empty()){bids_.erase(bestBidIt);}
        }
    }

    void addRestingOrder(const Order& o){
        BookSide& book = (o.side == Side::Buy) ? bids_: asks_;
        PriceLevel& lvl = book[o.price];
        lvl.orders.push_back(o);
        auto it = std::prev(lvl.orders.end());
        lvl.total_qty += o.qty;
        index_[o.id] = OrderLocator{o.side, o.price, it};
    }

    bool canFullyMatch(Side side, Price price, Qty qty) const{
        if(qty <= 0){return true;}
        Qty need = qty;
        if(side == Side::Buy){
            //need to hit asks <= price
            for(auto it = asks_.begin(); it != asks_.end(); ++it){
                Price px = it->first;
                if(px > price){break;} //too expensive
                need -= it->second.total_qty;
                if(need <= 0){return true;}
            }
        }
        else{ //side == sell
            //need to hit bids >= price
            for(auto it = bids_.rbegin(); it != bids_.rend(); ++it){
                Price px = it->first;
                if(px < price){break;} //too low
                need -= it->second.total_qty;
                if(need <= 0){return true;}
            }
        }
        return false;
    }
};
}