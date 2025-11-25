#include "Poller.h"
#include <sys/epoll.h>
#include <sys/poll.h>
#include <unistd.h>
#include <stdexcept>
#include <cassert>
#include "Channel.h"

#include "EpollPoller.h"

bool Poller::has_channel(const Channel* channel) const {
    // auto it = channels.find(channel->get_fd());
    // if (it != channels.end()) {
    //     assert(it->second == channel);
    //     return true;
    // }
    // else return false;
    return false; // base Poller does not have channels map
}

Poller* Poller::new_default_poller(EventLoop* loop) {
    /* if (::getenv("USE_POLL")) {
        /// TODO: add code, return poller()
        return nullptr;
    }
    else  */
    {
        return new EpollPoller(loop); // where to delete？
    }
}
