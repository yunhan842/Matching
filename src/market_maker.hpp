#pragma once

#include "matching_engine.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>

namespace matching{

struct MarketMakerConfig{
    std::string symbol{"MM"};
    UserId user_id{9001};
    Qty quote_qty{25};
    Price fair_value{100};
    Price half_spread_ticks{1};
    Qty max_inventory{150};
    Qty inventory_skew_step{50};
};

struct MarketMakerStats{
    Qty position{0};
    long long cash{0};
    Qty bid_filled_qty{0};
    Qty ask_filled_qty{0};
    std::uint64_t quote_updates{0};
};

class SimpleMarketMaker{
public:
    explicit SimpleMarketMaker(MarketMakerConfig config)
    : config_(std::move(config)) {}

    const MarketMakerStats& stats() const{return stats_;}

    bool onTrade(const Trade& trade){
        if(trade.symbol_name == nullptr || config_.symbol != trade.symbol_name){return false;}

        bool touched = false;
        if(bid_.active && trade.buy_id == bid_.id){
            applyBidFill(trade.price, trade.qty);
            touched = true;
        }
        if(ask_.active && trade.sell_id == ask_.id){
            applyAskFill(trade.price, trade.qty);
            touched = true;
        }
        return touched;
    }

    void onTick(MatchingEngine& engine){
        const TopOfBook tob = engine.topOfBook(config_.symbol);
        const Price fair = estimateFairValue(tob);
        const Price skew = inventorySkewTicks();

        bool quote_bid = stats_.position < config_.max_inventory;
        bool quote_ask = stats_.position > -config_.max_inventory;

        Price desired_bid = fair - config_.half_spread_ticks - skew;
        Price desired_ask = fair + config_.half_spread_ticks - skew;

        if(tob.best_ask && desired_bid >= *tob.best_ask){
            desired_bid = *tob.best_ask - 1;
        }
        if(tob.best_bid && desired_ask <= *tob.best_bid){
            desired_ask = *tob.best_bid + 1;
        }
        if(desired_bid >= desired_ask){
            desired_ask = desired_bid + 1;
        }

        maintainQuote(engine, bid_, Side::Buy, quote_bid, desired_bid);
        maintainQuote(engine, ask_, Side::Sell, quote_ask, desired_ask);
    }

    void cancelAll(MatchingEngine& engine){
        cancelQuote(engine, bid_);
        cancelQuote(engine, ask_);
    }

    long long markToMarket(const MatchingEngine& engine) const{
        return stats_.cash + static_cast<long long>(stats_.position) *
            static_cast<long long>(estimateFairValue(engine.topOfBook(config_.symbol)));
    }

    void printStatus(const MatchingEngine& engine, std::ostream& os) const{
        os << "MM status symbol=" << config_.symbol
           << " position=" << stats_.position
           << " cash=" << stats_.cash
           << " mtm_pnl=" << markToMarket(engine)
           << " bid_fill_qty=" << stats_.bid_filled_qty
           << " ask_fill_qty=" << stats_.ask_filled_qty
           << " quote_updates=" << stats_.quote_updates;

        if(bid_.active){
            os << " active_bid=" << bid_.price << "x" << bid_.remaining
               << "(id=" << bid_.id << ")";
        }
        if(ask_.active){
            os << " active_ask=" << ask_.price << "x" << ask_.remaining
               << "(id=" << ask_.id << ")";
        }
        os << "\n";
    }

private:
    struct ActiveQuote{
        bool active{false};
        OrderId id{0};
        Price price{0};
        Qty remaining{0};
    };

    MarketMakerConfig config_;
    MarketMakerStats stats_;
    ActiveQuote bid_;
    ActiveQuote ask_;

    Price estimateFairValue(const TopOfBook& tob) const{
        if(tob.mid_price){return *tob.mid_price;}
        if(tob.best_bid && tob.best_ask){return (*tob.best_bid + *tob.best_ask) / 2;}
        if(tob.best_bid){return *tob.best_bid + config_.half_spread_ticks;}
        if(tob.best_ask){return *tob.best_ask - config_.half_spread_ticks;}
        return config_.fair_value;
    }

    Price inventorySkewTicks() const{
        if(config_.inventory_skew_step <= 0){return 0;}
        return static_cast<Price>(stats_.position / config_.inventory_skew_step);
    }

    void maintainQuote(MatchingEngine& engine, ActiveQuote& quote, Side side,
                       bool should_quote, Price desired_price){
        if(!should_quote){
            cancelQuote(engine, quote);
            return;
        }
        if(quote.active && quote.price == desired_price && quote.remaining == config_.quote_qty){
            return;
        }

        cancelQuote(engine, quote);
        OrderId id = engine.newLimit(
            config_.symbol, config_.user_id, side, desired_price,
            config_.quote_qty, TimeInForce::GFD);
        if(id != 0){
            quote.active = true;
            quote.id = id;
            quote.price = desired_price;
            quote.remaining = config_.quote_qty;
            ++stats_.quote_updates;
        }
    }

    void cancelQuote(MatchingEngine& engine, ActiveQuote& quote){
        if(!quote.active){return;}
        engine.cancel(config_.symbol, quote.id);
        quote = ActiveQuote{};
    }

    void applyBidFill(Price price, Qty qty){
        stats_.position += qty;
        stats_.cash -= static_cast<long long>(price) * static_cast<long long>(qty);
        stats_.bid_filled_qty += qty;
        bid_.remaining -= qty;
        if(bid_.remaining <= 0){bid_ = ActiveQuote{};}
    }

    void applyAskFill(Price price, Qty qty){
        stats_.position -= qty;
        stats_.cash += static_cast<long long>(price) * static_cast<long long>(qty);
        stats_.ask_filled_qty += qty;
        ask_.remaining -= qty;
        if(ask_.remaining <= 0){ask_ = ActiveQuote{};}
    }
};

}
