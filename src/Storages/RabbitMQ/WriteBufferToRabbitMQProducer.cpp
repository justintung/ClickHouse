#include <Storages/RabbitMQ/WriteBufferToRabbitMQProducer.h>
#include "Core/Block.h"
#include "Columns/ColumnString.h"
#include "Columns/ColumnsNumber.h"
#include <common/logger_useful.h>
#include <amqpcpp.h>
#include <uv.h>
#include <chrono>
#include <thread>
#include <atomic>


namespace DB
{

enum
{
    Connection_setup_sleep = 200,
    Loop_retries_max = 1000,
    Loop_wait = 10,
    Batch = 10000
};

WriteBufferToRabbitMQProducer::WriteBufferToRabbitMQProducer(
        std::pair<String, UInt16> & parsed_address,
        std::pair<String, String> & login_password_,
        const String & routing_key_,
        const String exchange_,
        Poco::Logger * log_,
        const size_t num_queues_,
        const bool bind_by_id_,
        const bool use_transactional_channel_,
        std::optional<char> delimiter,
        size_t rows_per_message,
        size_t chunk_size_)
        : WriteBuffer(nullptr, 0)
        , login_password(login_password_)
        , routing_key(routing_key_)
        , exchange_name(exchange_ + "_direct")
        , log(log_)
        , num_queues(num_queues_)
        , bind_by_id(bind_by_id_)
        , use_transactional_channel(use_transactional_channel_)
        , delim(delimiter)
        , max_rows(rows_per_message)
        , chunk_size(chunk_size_)
{

    loop = new uv_loop_t;
    uv_loop_init(loop);

    event_handler = std::make_unique<RabbitMQHandler>(loop, log);
    connection = std::make_unique<AMQP::TcpConnection>(event_handler.get(), AMQP::Address(parsed_address.first, parsed_address.second, AMQP::Login(login_password.first, login_password.second), "/"));

    /* The reason behind making a separate connection for each concurrent producer is explained here:
     * https://github.com/CopernicaMarketingSoftware/AMQP-CPP/issues/128#issuecomment-300780086 - publishing from
     * different threads (as outputStreams are asynchronous) with the same connection leads to internal library errors.
     */
    size_t cnt_retries = 0;
    while (!connection->ready() && ++cnt_retries != Loop_retries_max)
    {
        uv_run(loop, UV_RUN_NOWAIT);
        std::this_thread::sleep_for(std::chrono::milliseconds(Connection_setup_sleep));
    }

    if (!connection->ready())
    {
        LOG_ERROR(log, "Cannot set up connection for producer!");
    }

    producer_channel = std::make_shared<AMQP::TcpChannel>(connection.get());
    checkExchange();

    /// If publishing should be wrapped in transactions
    if (use_transactional_channel)
    {
        producer_channel->startTransaction();
    }
}


WriteBufferToRabbitMQProducer::~WriteBufferToRabbitMQProducer()
{
    finilizeProducer();
    connection->close();
    event_handler->stop();

    assert(rows == 0 && chunks.empty());
}


void WriteBufferToRabbitMQProducer::countRow()
{
    if (++rows % max_rows == 0)
    {
        const std::string & last_chunk = chunks.back();
        size_t last_chunk_size = offset();

        if (delim && last_chunk[last_chunk_size - 1] == delim)
            --last_chunk_size;

        std::string payload;
        payload.reserve((chunks.size() - 1) * chunk_size + last_chunk_size);

        for (auto i = chunks.begin(), e = --chunks.end(); i != e; ++i)
            payload.append(*i);

        payload.append(last_chunk, 0, last_chunk_size);

        rows = 0;
        chunks.clear();
        set(nullptr, 0);

        next_queue = next_queue % num_queues + 1;

        if (bind_by_id)
        {
            producer_channel->publish(exchange_name, std::to_string(next_queue), payload);
        }
        else
        {
            producer_channel->publish(exchange_name, routing_key, payload);
        }

        ++message_counter;

        /* Run event loop to actually publish, checking exchange is just a point to stop the event loop. Messages are not sent
         * without looping and looping after every batch is much better than processing all the messages in one time.
         */
        if ((message_counter %= Batch) == 0)
        {
            checkExchange();
        }
    }
}


void WriteBufferToRabbitMQProducer::checkExchange()
{
    std::atomic<bool> exchange_declared = false, exchange_error = false;

    /* The AMQP::passive flag indicates that it should only be checked if there is a valid exchange with the given name
     * and makes it declared on the current producer_channel.
     */
    producer_channel->declareExchange(exchange_name, AMQP::direct, AMQP::passive)
    .onSuccess([&]()
    {
        exchange_declared = true;
    })
    .onError([&](const char * message)
    {
        exchange_error = true;
        LOG_ERROR(log, "Exchange for INSERT query was not declared. Reason: {}", message);
    });

    /// These variables are updated in a separate thread and starting the loop blocks current thread
    while (!exchange_declared && !exchange_error)
    {
        startEventLoop();
    }
}


void WriteBufferToRabbitMQProducer::finilizeProducer()
{
    checkExchange();

    if (use_transactional_channel)
    {
        std::atomic<bool> answer_received = false;
        producer_channel->commitTransaction()
        .onSuccess([&]()
        {
            answer_received = true;
            LOG_TRACE(log, "All messages were successfully published");
        })
        .onError([&](const char * message)
        {
            answer_received = true;
            LOG_TRACE(log, "None of messages were publishd: {}", message);
            /// Probably should do something here
        });

        size_t count_retries = 0;
        while (!answer_received && ++count_retries != Loop_retries_max)
        {
            startEventLoop();
            std::this_thread::sleep_for(std::chrono::milliseconds(Loop_wait));
        }
    }
}


void WriteBufferToRabbitMQProducer::nextImpl()
{
    chunks.push_back(std::string());
    chunks.back().resize(chunk_size);
    set(chunks.back().data(), chunk_size);
}


void WriteBufferToRabbitMQProducer::startEventLoop()
{
    event_handler->startProducerLoop();
}

}