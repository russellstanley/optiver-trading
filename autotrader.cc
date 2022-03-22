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

/*
TODO:
2. Adaptive pair-trading, aka buy and sell amount based on the ratio between the prices

3. Investigate the best prices to buy/sell.
*/

using namespace ReadyTraderGo;

RTG_INLINE_GLOBAL_LOGGER_WITH_CHANNEL(LG_AT, "AUTO")

constexpr int LOT_SIZE = 20;
constexpr int POSITION_LIMIT = 100;
constexpr int TICK_SIZE_IN_CENTS = 100;

constexpr float BUY_RATIO = 0.995;
constexpr float SELL_RATIO = 1.005;

constexpr float DECAY_RATE = 0.0001; // 0.01%
constexpr float DECAY_BUY_LIMIT = 0.998;
constexpr float DECAY_SELL_LIMIT = 1.002;

AutoTrader::AutoTrader(boost::asio::io_context &context) : BaseAutoTrader(context)
{
    // ratioFile.open("ratios.csv");
}

void AutoTrader::DisconnectHandler()
{
    // ratioFile.close();
    BaseAutoTrader::DisconnectHandler();
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

    setMidpoint(instrument, bidPrices[0], askPrices[0]);

    if (instrument == Instrument::ETF)
    {
        float ratio = (float)midpointETF / (float)midpointFuture;

        RLOG(LG_AT, LogLevel::LL_INFO) << "ratio: " << ratio;

        // Check if current pair trading opportunity has expired.
        if (mAskId != 0 && ratio <= 1)
        {
            SendCancelOrder(mAskId);
            RLOG(LG_AT, LogLevel::LL_INFO) << "sell order " << mAskId << " cancelled ";
            mAskId = 0;
        }
        if (mBidId != 0 && ratio >= 1)
        {
            SendCancelOrder(mBidId);
            RLOG(LG_AT, LogLevel::LL_INFO) << "buy order " << mBidId << " cancelled ";
            mBidId = 0;
        }

        RLOG(LG_AT, LogLevel::LL_INFO) << "position: " << mPosition;

        // Check if a pair trading opportunity exists.
        if (mBidId == 0 && ratio < BUY_RATIO && mPosition < POSITION_LIMIT)
        {
            int volume = setVolume(ratio, Side::BUY);
            mBidId = mNextMessageId++;
            SendInsertOrder(mBidId, Side::BUY, askPrices[0], volume, Lifespan::GOOD_FOR_DAY);
            RLOG(LG_AT, LogLevel::LL_INFO) << "sending buy order " << mBidId
                                           << " volume: " << volume
                                           << " bid price: " << midpointFuture;
            mBids.emplace(mBidId);
        }

        if (mAskId == 0 && ratio > SELL_RATIO && mPosition > -POSITION_LIMIT)
        {
            int volume = setVolume(ratio, Side::SELL);
            mAskId = mNextMessageId++;
            SendInsertOrder(mAskId, Side::SELL, bidPrices[0], volume, Lifespan::GOOD_FOR_DAY);
            RLOG(LG_AT, LogLevel::LL_INFO) << "sending sell order " << mAskId
                                           << " volume: " << volume
                                           << " ask price: " << midpointFuture;
            mAsks.emplace(mAskId);
        }
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

void AutoTrader::setMidpoint(Instrument instrument,
                             unsigned long bidPrice,
                             unsigned long askPrice)
{
    // Check for undefined division.
    if (bidPrice == 0 || askPrice == 0)
    {
        return;
    }

    if (instrument == Instrument::FUTURE)
    {
        midpointFuture = (askPrice + bidPrice) / 2;
        if (midpointFuture % 100 != 0)
        {
            midpointFuture += 50;
        }
    }

    if (instrument == Instrument::ETF)
    {
        midpointETF = (askPrice + bidPrice) / 2;
        if (midpointETF % 100 != 0)
        {
            midpointETF += 50;
        }
    }
}

int AutoTrader::setVolume(float ratio, Side side)
{
    int volume = LOT_SIZE;

    // Check if max ratio shoud be updated or decreased
    if (ratio > maxRatio)
    {
        maxRatio = ratio;
        RLOG(LG_AT, LogLevel::LL_INFO) << "Opportunity Detected - "
                                       << "Position: " << mPosition;
        volume = LOT_SIZE * 2;
    }
    else if (maxRatio >= DECAY_SELL_LIMIT)
    {
        maxRatio = maxRatio - (maxRatio * DECAY_RATE);
    }

    // Check if min ratio should be updated
    if (ratio < minRatio)
    {
        minRatio = ratio;
        RLOG(LG_AT, LogLevel::LL_INFO) << "Opportunity Detected - "
                                       << "Position: " << mPosition;
        volume = LOT_SIZE * 2;
    }
    else if (minRatio <= DECAY_BUY_LIMIT)
    {
        minRatio = minRatio + (minRatio * DECAY_RATE);
    }

    // Check volume does not exeed our positon
    if (side == Side::SELL && mPosition - volume <= -POSITION_LIMIT)
    {
        volume = POSITION_LIMIT - abs(mPosition);
    }
    else if (side == Side::BUY && mPosition + volume >= POSITION_LIMIT)
    {
        volume = POSITION_LIMIT - abs(mPosition);
    }

    RLOG(LG_AT, LogLevel::LL_INFO) << "position: " << mPosition
                                   << " volume: " << volume;

    // ratioFile << maxRatio << "," << ratio << "," << minRatio << "\n";

    return volume;
}
