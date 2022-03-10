// Copyright 2021 Optiver Asia Pacific Pty. Ltd.
//
// This file is part of Ready Trader Go.
//
//     Ready Trader Go is free software: you can redistribute it and/or
//     modify it under the terms of the GNU Affero General Public License
//     as published by the Free Software Foundation, either version 3 of
//     the License, or (at your option) any later version.
//
//     Ready Trader Go is distributed in the hope that it will be useful,
//     but WITHOUT ANY WARRANTY; without even the implied warranty of
//     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//     GNU Affero General Public License for more details.
//
//     You should have received a copy of the GNU Affero General Public
//     License along with Ready Trader Go.  If not, see
//     <https://www.gnu.org/licenses/>.
#include <array>

#include <boost/asio/io_context.hpp>

#include <ready_trader_go/logging.h>

#include "autotrader.h"

using namespace ReadyTraderGo;

RTG_INLINE_GLOBAL_LOGGER_WITH_CHANNEL(LG_AT, "AUTO")

constexpr int LOT_SIZE = 10;
constexpr int POSITION_LIMIT = 100;
constexpr int TICK_SIZE_IN_CENTS = 100;

/* IDEAS

Increase logging
Measure the bid-ask spread
Implement a way to ammend orders
Find out what the Order Message Handler is doing. Pairs Trading, Market Making, something else??

*/

AutoTrader::AutoTrader(boost::asio::io_context &context) : BaseAutoTrader(context)
{
    ratioFile.open("ratios.csv");
}

void AutoTrader::DisconnectHandler()
{
    BaseAutoTrader::DisconnectHandler();
    ratioFile.close();
    RLOG(LG_AT, LogLevel::LL_INFO) << "execution connection lost";
}

void AutoTrader::ErrorMessageHandler(unsigned long clientOrderId,
                                     const std::string &errorMessage)
{
    RLOG(LG_AT, LogLevel::LL_INFO) << "error with order " << clientOrderId << ": " << errorMessage;
    if (clientOrderId != 0)
    {
        OrderStatusMessageHandler(clientOrderId, 0, 0, 0);
    }
}

void AutoTrader::HedgeFilledMessageHandler(unsigned long clientOrderId,
                                           unsigned long price,
                                           unsigned long volume)
{
    RLOG(LG_AT, LogLevel::LL_INFO) << "hedge order " << clientOrderId << " filled for " << volume
                                   << " lots at $" << price << " average price in cents";
}

void AutoTrader::OrderBookMessageHandler(Instrument instrument,
                                         unsigned long sequenceNumber,
                                         const std::array<unsigned long, TOP_LEVEL_COUNT> &askPrices,
                                         const std::array<unsigned long, TOP_LEVEL_COUNT> &askVolumes,
                                         const std::array<unsigned long, TOP_LEVEL_COUNT> &bidPrices,
                                         const std::array<unsigned long, TOP_LEVEL_COUNT> &bidVolumes)
{
    RLOG(LG_AT, LogLevel::LL_INFO) << "order book received for " << instrument << " instrument"
                                   << ": ask prices: " << askPrices[0]
                                   << "; ask volumes: " << askVolumes[0]
                                   << "; bid prices: " << bidPrices[0]
                                   << "; bid volumes: " << bidVolumes[0];

    if (instrument == Instrument::ETF)
    {
        midpointETF = (askPrices[0] + bidPrices[0]) / 2;
    }

    if (instrument == Instrument::FUTURE)
    {
        unsigned long priceAdjustment = -(mPosition / LOT_SIZE) * TICK_SIZE_IN_CENTS;
        unsigned long newAskPrice = (askPrices[0] != 0) ? askPrices[0] + priceAdjustment : 0;
        unsigned long newBidPrice = (bidPrices[0] != 0) ? bidPrices[0] + priceAdjustment : 0;

        midpointFuture = (askPrices[0] + bidPrices[0]) / 2;
        float ratio = midpointETF / midpointFuture;
        ratioFile << ratio << "\n";
        RLOG(LG_AT, LogLevel::LL_INFO) << "ratio: " << ratio;

        /*
        if (mAskId != 0 && newAskPrice != 0 && newAskPrice != mAskPrice)
        {
            SendCancelOrder(mAskId);
            RLOG(LG_AT, LogLevel::LL_INFO) << "sell order " << mAskId << " cancelled ";
            mAskId = 0;
        }
        if (mBidId != 0 && newBidPrice != 0 && newBidPrice != mBidPrice)
        {
            SendCancelOrder(mBidId);
            RLOG(LG_AT, LogLevel::LL_INFO) << "buy order " << mBidId << " cancelled ";
            mBidId = 0;
        }
        */

        if (mBidId == 0 && ratio > 1.005)
        {
            mBidId = mNextMessageId++;
            mBidPrice = bidPrices[0];
            SendInsertOrder(mBidId, Side::BUY, mBidPrice, LOT_SIZE, Lifespan::GOOD_FOR_DAY);
            mBids.emplace(mBidId);
        }
        if (mAskId == 0 && ratio < 0.995)
        {
            mAskId = mNextMessageId++;
            mAskPrice = askPrices[0];
            SendInsertOrder(mAskId, Side::SELL, mAskPrice, LOT_SIZE, Lifespan::GOOD_FOR_DAY);
            mAsks.emplace(mAskId);
        }

        /*
        if (mAskId == 0 && newAskPrice != 0 && mPosition > -POSITION_LIMIT)
        {
            mAskId = mNextMessageId++;
            mAskPrice = newAskPrice;
            SendInsertOrder(mAskId, Side::SELL, newAskPrice, LOT_SIZE, Lifespan::GOOD_FOR_DAY);
            mAsks.emplace(mAskId);
        }
        if (mBidId == 0 && newBidPrice != 0 && mPosition < POSITION_LIMIT)
        {
            mBidId = mNextMessageId++;
            mBidPrice = newBidPrice;
            SendInsertOrder(mBidId, Side::BUY, newBidPrice, LOT_SIZE, Lifespan::GOOD_FOR_DAY);
            mBids.emplace(mBidId);
        }
        */
    }
}

void AutoTrader::OrderFilledMessageHandler(unsigned long clientOrderId,
                                           unsigned long price,
                                           unsigned long volume)
{
    RLOG(LG_AT, LogLevel::LL_INFO) << "order " << clientOrderId << " filled for " << volume
                                   << " lots at $" << price << " cents";
    if (mAsks.count(clientOrderId) == 1)
    {
        mPosition -= (long)volume;
        SendHedgeOrder(mNextMessageId++, Side::BUY, MAXIMUM_ASK / TICK_SIZE_IN_CENTS * TICK_SIZE_IN_CENTS, volume);
    }
    else if (mBids.count(clientOrderId) == 1)
    {
        mPosition += (long)volume;
        SendHedgeOrder(mNextMessageId++, Side::SELL, MINIMUM_BID, volume);
    }
}

void AutoTrader::OrderStatusMessageHandler(unsigned long clientOrderId,
                                           unsigned long fillVolume,
                                           unsigned long remainingVolume,
                                           signed long fees)
{
    RLOG(LG_AT, LogLevel::LL_INFO) << "order " << clientOrderId << " was updated. filled: " << fillVolume
                                   << " remaining: " << remainingVolume
                                   << " fees: " << fees;
    if (remainingVolume == 0)
    {
        if (clientOrderId == mAskId)
        {
            mAskId = 0;
        }
        else if (clientOrderId == mBidId)
        {
            mBidId = 0;
        }

        mAsks.erase(clientOrderId);
        mBids.erase(clientOrderId);
    }
}

void AutoTrader::TradeTicksMessageHandler(Instrument instrument,
                                          unsigned long sequenceNumber,
                                          const std::array<unsigned long, TOP_LEVEL_COUNT> &askPrices,
                                          const std::array<unsigned long, TOP_LEVEL_COUNT> &askVolumes,
                                          const std::array<unsigned long, TOP_LEVEL_COUNT> &bidPrices,
                                          const std::array<unsigned long, TOP_LEVEL_COUNT> &bidVolumes)
{
    RLOG(LG_AT, LogLevel::LL_INFO) << "trade ticks received for " << instrument << " instrument"
                                   << ": ask prices: " << askPrices[0]
                                   << "; ask volumes: " << askVolumes[0]
                                   << "; bid prices: " << bidPrices[0]
                                   << "; bid volumes: " << bidVolumes[0];
}
