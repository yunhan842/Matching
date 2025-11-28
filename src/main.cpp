#include "matching_engine.hpp"
#include "async_matching_engine.hpp"
#include "protocol.hpp"
#include <iostream>
#include <random>
#include <chrono>
#include <vector>
#include <fstream>
#include <unordered_set>

using namespace matching;

void runBenchmark(std::size_t num_events){
    std::uint64_t trade_count = 0;
    std::uint64_t traded_qty  = 0;

    MatchingEngine engine([&](const Trade& t){
        ++trade_count;
        traded_qty += t.qty;
    });

    engine.reserveOwnerMap(num_events);

    std::mt19937_64 rng(12345);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<int> price_dist(95, 105);
    std::uniform_int_distribution<int> qty_dist(1, 100);
    std::uniform_int_distribution<int> cancel_dist(0, 9);

    const std::string symbol = "FOO";
    std::vector<OrderId> live_orders;
    live_orders.reserve(num_events);

    auto t0 = std::chrono::steady_clock::now();

    for(std::size_t i = 0; i < num_events; ++i){
        if(!live_orders.empty() && cancel_dist(rng) == 0){
            //cancel event
            std::uniform_int_distribution<std::size_t> idx_dist(0, live_orders.size() - 1);
            std::size_t idx = idx_dist(rng);
            OrderId id = live_orders[idx];

            Event e;
            e.type   = EventType::Cancel;
            e.symbol = symbol;
            e.id     = id;
            engine.process(e);

            live_orders[idx] = live_orders.back();
            live_orders.pop_back();
        } 
        else{
            //new limit
            Event e;
            e.type = EventType::NewLimit;
            e.symbol = symbol;
            e.side = (side_dist(rng) == 0 ? Side::Buy : Side::Sell);
            e.price = price_dist(rng);
            e.qty = qty_dist(rng);

            OrderId id = engine.newLimit(e.symbol, e.side, e.price, e.qty);
            live_orders.push_back(id);
        }
    }

    auto t1 = std::chrono::steady_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    double seconds = ns / 1e9;

    std::cout << "Processed " << num_events << " events in "
              << seconds << " s, ~"
              << (num_events / seconds / 1e6) << " M events/s\n";

    auto tob = engine.topOfBook("FOO");
    std::cout << "FOO "
            << "bid=" << (tob.best_bid ? std::to_string(*tob.best_bid) : "none")
            << " x "  << (tob.bid_size ? std::to_string(*tob.bid_size) : "0")
            << "   ask=" << (tob.best_ask ? std::to_string(*tob.best_ask) : "none")
            << " x "    << (tob.ask_size ? std::to_string(*tob.ask_size) : "0");
    if (tob.mid_price) {
        std::cout << "   mid=" << *tob.mid_price;
    }
    std::cout << "\n";

    std::cout << "Trades executed: " << trade_count
              << ", total traded qty = " << traded_qty << "\n";
}

void runAsyncBenchmark(std::size_t num_events){
    std::uint64_t trade_count = 0;
    std::uint64_t traded_qty = 0;

    AsyncMatchingEngine async_eng([&](const Trade& t){
        ++trade_count;
        traded_qty += t.qty;
    });

    async_eng.engine().reserveOwnerMap(num_events);

    std::mt19937_64 rng(12345);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<int> price_dist(95, 105);
    std::uniform_int_distribution<int> qty_dist(1, 100);
    std::uniform_int_distribution<int> cancel_dist(0, 9);

    const std::string symbol = "FOO";
    std::vector<OrderId> live_orders;
    live_orders.reserve(num_events);

    auto t0 = std::chrono::steady_clock::now();

    for(std::size_t i = 0; i < num_events; ++i){
        Event e{};
        if(!live_orders.empty() && cancel_dist(rng) == 0){
            //cancel
            std::uniform_int_distribution<std::size_t> idx_dist(0, live_orders.size() - 1);
            std::size_t idx = idx_dist(rng);
            OrderId id = live_orders[idx];

            e.type = EventType::Cancel;
            e.symbol = symbol;
            e.id = id;
            async_eng.submit(e);
            live_orders[idx] = live_orders.back();
            live_orders.pop_back();
        }
        else{
            //new limit
            e.type = EventType::NewLimit;
            e.symbol = symbol;
            e.side = (side_dist(rng) == 0 ? Side::Buy: Side::Sell);
            e.price = price_dist(rng);
            e.qty = qty_dist(rng);
            e.tif = TimeInForce::GFD;
            async_eng.submit(e);
            //we don't get the new order id back via submit(), so for this
            //toy benchmark we’ll just track cancels less accurately and
            //focus on throughput. A more “correct” version would have
            //an ack path carrying assigned IDs back to the producer.
        }
    }
    //tell the worker to finish processing and join it
    async_eng.stop();
    auto t1 = std::chrono::steady_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    double seconds = ns / 1e9;

    std::cout << "--- Async benchmark ---\n";
    std::cout << "Processed " << num_events << " events in "
              << seconds << " s, ~"
              << (num_events / seconds / 1e6) << " M events/s\n";

    if(auto stats = async_eng.engine().bookStats(symbol)){
        std::cout << "FOO trades=" << stats->trade_count
                  << " volume="   << stats->traded_qty;
        if(stats->has_last_trade){
            std::cout << " last_px=" << stats->last_trade_price;
        }
        std::cout << "\n";
    }

    std::cout << "Trades executed: " << trade_count
              << ", total traded qty = " << traded_qty << "\n";
}

void runInteractive(){
    using namespace matching;
    std::cout << "\n--- Interactive mode (type commands, Ctrl+D to exit) ---\n";
    std::cout << "Formats:\n"
              << "  L,symbol,B|S,price,qty,GFD|IOC|FOK\n"
              << "  M,symbol,B|S,qty\n"
              << "  C,symbol,orderId\n"
              << "  R,symbol,oldId,B|S,price,qty,GFD|IOC|FOK\n\n";

    AsyncMatchingEngine async_eng([](const Trade& t){
        std::cout << "TRADE " << t.symbol
                  << " px="  << t.price
                  << " qty=" << t.qty
                  << " buy=" << t.buy_id
                  << " sell="<< t.sell_id
                  << "\n";
    });

    std::string line;
    while(std::getline(std::cin, line)){
        std::string trimmed = matching::trim(line);

        if(trimmed == "q" || trimmed == "Q" ||
            trimmed == "quit" || trimmed == "QUIT"){
            std::cout << "Stopping order input.\n";
            break;
        }
        Event e{};
        if (!parseLine(line, e)) {
            continue; //error already printed, skip
        }
        async_eng.submit(e);

        //as a small demo, after every command print top-of-book for that symbol
        auto tob = async_eng.engine().topOfBook(e.symbol);
        std::cout << e.symbol << " bid="
                  << (tob.best_bid ? std::to_string(*tob.best_bid) : "none")
                  << " x " << (tob.bid_size ? std::to_string(*tob.bid_size) : "0")
                  << "   ask="
                  << (tob.best_ask ? std::to_string(*tob.best_ask) : "none")
                  << " x " << (tob.ask_size ? std::to_string(*tob.ask_size) : "0")
                  << "\n";
    }

    async_eng.stop();
}

void runInteractiveSync(){
    using namespace matching;

    std::cout << "\n--- Interactive mode (sync) ---\n";
    std::cout << "Formats:\n"
              << "  L,symbol,B|S,price,qty,GFD|IOC|FOK\n"
              << "  M,symbol,B|S,qty\n"
              << "  C,symbol,orderId\n"
              << "  R,symbol,oldId,B|S,price,qty,GFD|IOC|FOK\n\n";
    
    //open logs in append mode so you can accumulate sessions if you want
    std::ofstream eventLog("events.log", std::ios::app);
    std::ofstream tradeLog("trades.log", std::ios::app);
    if(!eventLog || !tradeLog){
        std::cerr << "Error: could not open events.log or trades.log for writing\n";
        return;
    }

    MatchingEngine engine([&](const Trade& t){
        std::cout << "TRADE " << t.symbol
                  << " px="  << t.price
                  << " qty=" << t.qty
                  << " buy=" << t.buy_id
                  << " sell="<< t.sell_id
                  << "\n";
        //log as csv: T,symbol,price,qty,buyId,sellId
        tradeLog << "T," << t.symbol << ","
                 << t.price << ","
                 << t.qty << ","
                 << t.buy_id << ","
                 << t.sell_id << "\n";
        tradeLog.flush();
    });

    std::string line;
    while(std::getline(std::cin , line)){
        std::string trimmed = matching::trim(line);
        if(trimmed.empty()){continue;}

        //log raw line first (including D commands)
        eventLog << trimmed << "\n";
        eventLog.flush();
        
        //depth command: D,symbol[,depth]
        if(!trimmed.empty() && trimmed[0] == 'D'){
            auto fields = matching::splitCSV(trimmed);
            if(fields.size() < 2 || fields.size() > 3){
                std::cerr <<"Invalid D line: " << trimmed << "\n";
                continue;
            }
            std::string symbol = matching::trim(fields[1]);
            int depth = 5;
            if(fields.size() == 3){
                try{
                    depth = std::stoi(matching::trim(fields[2]));
                    if(depth <= 0){depth = 5;}
                } catch(...){
                    std::cerr << "Invalid depth in D lines: " << trimmed << "\n";
                    depth = 5;
                }
            }
            const OrderBook* book = engine.findBook(symbol);
            if(!book){std::cout << "No book for symbol: " << symbol << "\n";}
            else{book->printBook(std::cout, depth);}
            continue;
        }

        if(!trimmed.empty() && trimmed[0] == 'U'){
            auto fields = matching::splitCSV(trimmed);
            if(fields.size() != 3){
                std::cerr <<"Invalid U line: " << trimmed << "\n";
                continue;
            }
            try{
                UserId user = static_cast<UserId>(std::stoll(matching::trim(fields[1])));
                std::string symbol = matching::trim(fields[2]);

                auto posOpt = engine.userPositions(user, symbol);
                if(!posOpt){std::cout << "User " << user << " has no position in " << symbol << "\n";}
                else{
                    auto pos = *posOpt;
                    std::cout << "User " << user << " " << symbol
                              << " position=" << pos.position
                              << " traded_volume=" << pos.traded_volume << "\n";
                }
            } catch(...){std::cerr << "Invalid user id in U line: " << trimmed << "\n";}
            continue;
        }

        if(trimmed == "q" || trimmed == "Q" ||
            trimmed == "quit" || trimmed == "QUIT"){
            std::cout << "Stopping order input.\n";
            break;
        }
        //normal L/M/C/R line → parse into Event and apply to engine
        Event e{};
        if(!parseLine(trimmed, e)){continue;}

        //handle each type explicitly so we can print ACKs with IDs
        switch(e.type){
        case EventType::NewLimit:{
            OrderId id = engine.newLimit(e.symbol, e.user_id, e.side, e.price, e.qty, e.tif);
            std::cout << "ACK L id=" << id << " symbol=" << e.symbol
                      << " side=" << (e.side == Side::Buy ? "B" : "S")
                      << " px=" << e.price << " qty=" << e.qty
                      << " tif=" << (e.tif == TimeInForce::GFD ? "GFD" :
                                     e.tif == TimeInForce::IOC ? "IOC" : "FOK")
                      << "\n";
            break;
        }
        case EventType::NewMarket:{
            OrderId id = engine.newMarket(e.symbol, e.user_id, e.side, e.qty);
            std::cout << "ACK M id=" << id << " symbol=" << e.symbol
                      << " side=" << (e.side == Side::Buy ? "B" : "S")
                      << " qty=" << e.qty << "\n";
            break;
        }
        case EventType::Cancel:{
            bool ok = engine.cancel(e.symbol, e.id);
            std::cout << (ok ? "ACK " : "REJECT ")
                      << "C id=" << e.id << " symbol=" << e.symbol << "\n";
            break;
        }
        case EventType::Replace:{
            OrderId newId = engine.replace(e.symbol, e.id, e.side, e.price, e.qty, e.tif);
            std::cout << "ACK R old_id=" << e.id << " new_id=" << newId
                      << " symbol=" << e.symbol << "\n";
            break;
        }
        }
        auto tob = engine.topOfBook(e.symbol);
        std::cout << e.symbol << " bid="
                  << (tob.best_bid ? std::to_string(*tob.best_bid) : "none")
                  << " x " << (tob.bid_size ? std::to_string(*tob.bid_size) : "0")
                  << "   ask="
                  << (tob.best_ask ? std::to_string(*tob.best_ask) : "none")
                  << " x " << (tob.ask_size ? std::to_string(*tob.ask_size) : "0");
        if(tob.mid_price){
            std::cout << "   mid=" << *tob.mid_price;
        }
        std::cout << "\n";
    }
}

void runReplay(const std::string& filename){
    using namespace matching;

    std::ifstream in(filename);
    if(!in){
        std::cerr << "ERROR: cannot open replay file: " << filename << "\n";
        return;
    }
    MatchingEngine engine([](const Trade& t){
        (void)t;
    });
    std::string line;
    //track which symbols we see so we can print a summary later
    std::unordered_set<std::string> symbols;

    while(std::getline(in, line)){
        matching::Event e{};
        if(!parseLine(line, e)){continue;}
        symbols.insert(e.symbol);
        engine.process(e);
    }
    std::cout << "\n--- Replay summary for file: " << filename << " ---\n";
    for(const auto& sym : symbols){
        auto tob = engine.topOfBook(sym);
        std::cout << sym << " bid="
                  << (tob.best_bid ? std::to_string(*tob.best_bid) : "none")
                  << " x " << (tob.bid_size ? std::to_string(*tob.bid_size) : "0")
                  << "   ask="
                  << (tob.best_ask ? std::to_string(*tob.best_ask) : "none")
                  << " x " << (tob.ask_size ? std::to_string(*tob.ask_size) : "0");
        if(tob.mid_price){
            std::cout << "   mid=" << *tob.mid_price;
        }
        std::cout << "\n";

        if(auto stats = engine.bookStats(sym)){
            std::cout << "  trades=" << stats->trade_count
                      << " volume=" << stats->traded_qty;
            if(stats->has_last_trade){
                std::cout << " last_px=" << stats->last_trade_price;
            }
            std::cout << "\n";
        }
    }
}

int main(int argc, char** argv){
    using namespace matching;

    if(argc >= 3 && std::string(argv[1]) == "--replay"){
        runReplay(argv[2]);
        return 0;
    }

    MatchingEngine engine([](const Trade& t){
        std::cout << "TRADE symbol=" << t.symbol
                  << " px="  << t.price
                  << " qty=" << t.qty
                  << " buy=" << t.buy_id
                  << " sell="<< t.sell_id
                  << "\n";
    });

    //simple FOO demo using process(Event)
    {
        Event e1{EventType::NewLimit, "FOO", Side::Sell, 100, 50, 0};
        Event e2{EventType::NewLimit, "FOO", Side::Sell, 100, 60, 0};
        Event e3{EventType::NewLimit, "FOO", Side::Buy,  100, 80, 0};

        engine.process(e1);
        engine.process(e2);
        engine.process(e3);

        auto tob = engine.topOfBook("FOO");
        std::cout << "FOO "
                << "bid=" << (tob.best_bid ? std::to_string(*tob.best_bid) : "none")
                << " x "  << (tob.bid_size ? std::to_string(*tob.bid_size) : "0")
                << "   ask=" << (tob.best_ask ? std::to_string(*tob.best_ask) : "none")
                << " x "    << (tob.ask_size ? std::to_string(*tob.ask_size) : "0");
        if (tob.mid_price) {
            std::cout << "   mid=" << *tob.mid_price;
        }
        std::cout << "\n";


        if (auto* book = engine.findBook("FOO")) {
            book->printBook(std::cout, 5);
        }

        //cancel second ask (id 2 in this simple script)
        engine.cancel("FOO", 2);

        tob = engine.topOfBook("FOO");
        std::cout << "FOO after cancel bestBid="
                  << (tob.best_bid ? std::to_string(*tob.best_bid) : "none")
                  << " bestAsk="
                  << (tob.best_ask ? std::to_string(*tob.best_ask) : "none")
                  << "\n";

        if (auto* book = engine.findBook("FOO")) {
            book->printBook(std::cout, 5);
        }
    }

        //ioc test
    {
        std::cout << "\n--- IOC test (BAR) ---\n";
        engine.newLimit("BAR", Side::Sell, 100, 50); //rest ask 100 x 50

        //IOC buy for 80 @ 100. Trades 50, leftover 30 is dropped
        engine.newLimit("BAR", Side::Buy, 100, 80, TimeInForce::IOC);

        auto tob = engine.topOfBook("BAR");
        std::cout << "BAR bestBid="
                  << (tob.best_bid ? std::to_string(*tob.best_bid) : "none")
                  << " bestAsk="
                  << (tob.best_ask ? std::to_string(*tob.best_ask) : "none")
                  << "\n";
        //expected: ask side empty (we fully consumed that 50), bid none.
    }

    //fok test
    {
        std::cout << "\n--- FOK test (BAZ) ---\n";
        engine.newLimit("BAZ", Side::Sell, 100, 50); //rest ask 100 x 50

        //FOK buy for 80 @ 100 → cannot fully fill (only 50 available)
        //expect: NO trades, book unchanged
        engine.newLimit("BAZ", Side::Buy, 100, 80, TimeInForce::FOK);

        auto tob1 = engine.topOfBook("BAZ");
        std::cout << "After FOK(80) BAZ bestBid="
                  << (tob1.best_bid ? std::to_string(*tob1.best_bid) : "none")
                  << " bestAsk="
                  << (tob1.best_ask ? std::to_string(*tob1.best_ask) : "none")
                  << "\n";

        //now FOK buy for 40 @ 100 → can fully fill (40 <= 50)
        //expect: 1 trade of 40, remaining ask qty = 10
        engine.newLimit("BAZ", Side::Buy, 100, 40, TimeInForce::FOK);

        auto tob2 = engine.topOfBook("BAZ");
        std::cout << "After FOK(40) BAZ bestBid="
                  << (tob2.best_bid ? std::to_string(*tob2.best_bid) : "none")
                  << " bestAsk="
                  << (tob2.best_ask ? std::to_string(*tob2.best_ask) : "none")
                  << "\n";
    }

        //replace test (qux)
    {
        std::cout << "\n--- Replace test (QUX) ---\n";
        MatchingEngine eng2([](const Trade& t) {
            std::cout << "TRADE symbol=" << t.symbol
                      << " px="  << t.price
                      << " qty=" << t.qty
                      << " buy=" << t.buy_id
                      << " sell="<< t.sell_id
                      << "\n";
        });

        //initial resting ask: 100 x 50, id1
        OrderId id1 = eng2.newLimit("QUX", Side::Sell, 100, 50);

        //replace: move that ask up to 102 x 30 (simplified: cancel+new)
        Event r{};
        r.type = EventType::Replace;
        r.symbol = "QUX";
        r.id = id1;           //old order id
        r.side = Side::Sell;    //new side/price/qty
        r.price = 102;
        r.qty = 30;
        r.tif = TimeInForce::GFD;
        eng2.process(r);

        //aggressive buy @ 101 should NOT hit anything (ask now 102)
        eng2.newLimit("QUX", Side::Buy, 101, 100);

        auto tob = eng2.topOfBook("QUX");
        std::cout << "QUX bestBid="
                  << (tob.best_bid ? std::to_string(*tob.best_bid) : "none")
                  << " bestAsk="
                  << (tob.best_ask ? std::to_string(*tob.best_ask) : "none")
                  << "\n";
        //expected: bestAsk=102, bestBid=101
    }

    if (auto stats = engine.bookStats("FOO")) {
        std::cout << "FOO trades=" << stats->trade_count
                  << " volume="    << stats->traded_qty;
        if (stats->has_last_trade) {
            std::cout << " last_px=" << stats->last_trade_price;
        }
        std::cout << "\n";
    }

        //async engine demo
    {
        std::cout << "\n--- Async engine demo (ASY) ---\n";

        AsyncMatchingEngine async_eng([](const Trade& t) {
            std::cout << "ASY TRADE symbol=" << t.symbol
                      << " px="  << t.price
                      << " qty=" << t.qty
                      << " buy=" << t.buy_id
                      << " sell="<< t.sell_id
                      << "\n";
        });

        //producer thread (here: same thread for simplicity)
        Event e1{EventType::NewLimit, "ASY", Side::Sell, 100, 50, 0, TimeInForce::GFD};
        Event e2{EventType::NewLimit, "ASY", Side::Buy,  100, 50, 0, TimeInForce::GFD};

        async_eng.submit(e1);
        async_eng.submit(e2);

        //give the worker a moment to process
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        auto tob = async_eng.engine().topOfBook("ASY");
        std::cout << "ASY bid="
                  << (tob.best_bid ? std::to_string(*tob.best_bid) : "none")
                  << " x " << (tob.bid_size ? std::to_string(*tob.bid_size) : "0")
                  << "   ask="
                  << (tob.best_ask ? std::to_string(*tob.best_ask) : "none")
                  << " x " << (tob.ask_size ? std::to_string(*tob.ask_size) : "0")
                  << "\n";

        async_eng.stop();
    }

    std::cout << "\n--- Running benchmark ---\n";
    runBenchmark(1'000'000);

    std::cout << "\n--- Running async benchmark ---\n";
    runAsyncBenchmark(1'000'000);

    //runInteractive();

    std::cout << "\n";

    runInteractiveSync();

    return 0;
}