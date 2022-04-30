/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4; fill-column: 100 -*- */
/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <config.h>

#include <chrono>

#include "HttpRequest.hpp"
#include "Util.hpp"
#include "lokassert.hpp"

#include <WopiTestServer.hpp>
#include <Log.hpp>
#include <Unit.hpp>
#include <UnitHTTP.hpp>
#include <cstddef>
#include <helpers.hpp>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Util/LayeredConfiguration.h>

/// Test slow saving/uploading.
/// We modify the document, save, and immediately
/// modify again followed by closing the connection.
/// In this scenario, it's not just that the document
/// is modified at the time of unloading, which is
/// covered by the UnitWOPIAsncUpload_ModifyClose
/// test. Instead, here we close the connection
/// while the document is being saved and uploaded.
/// Unlike the failed upload scenario, this one
/// will hit "upload in progress" and will test
/// that in such a case we don't drop the latest
/// changes, which were done while save/upload
/// were in progress.
/// Modify, Save, Modify, Close -> No data loss.
class UnitWOPISlow : public WopiTestServer
{
    STATE_ENUM(Phase, Load, WaitLoadStatus, WaitModifiedStatus, WaitPutFile) _phase;

    static constexpr auto LargeDocumentFilename = "large-six-hundred.odt";

    /// The delay to simulate a slow server.
    std::chrono::milliseconds _serverResponseDelay;

    /// The number of key input sent.
    std::size_t _inputCount;

public:
    UnitWOPISlow()
        : WopiTestServer("UnitWOPISlow")
        , _phase(Phase::Load)
        , _serverResponseDelay(std::chrono::seconds(5))
        , _inputCount(0)
    {
        // We need more time than the default.
        setTimeout(std::chrono::minutes(10));

        // Read the document data and store as string in memory.
        const auto data = helpers::readDataFromFile(LargeDocumentFilename);
        setFileContent(Util::toString(data));
    }

    /// Given a URI, returns the filename.
    std::string getFilename(const Poco::URI& uri) const override
    {
        return extractFilenameFromWopiUri(uri.getPath());
    }

    std::unique_ptr<http::Response>
    assertPutFileRequest(const Poco::Net::HTTPRequest& request) override
    {
        LOG_TST("PutFile");
        LOK_ASSERT_MESSAGE("Expected to be in Phase::WaitPutFile", _phase == Phase::WaitPutFile);

        // Triggered while closing.
        LOK_ASSERT_EQUAL(std::string("false"), request.get("X-LOOL-WOPI-IsAutosave"));

        // Unfortunately, we clobber the modified flag when uploading.
        // So, if we had a user-modified upload that failed, the subsequent
        // try will have dropped the modified flag, and this assertion will fail.
        //FIXME: do not clobber the storage flags (modified, forced, etc.) when retrying.
        // LOK_ASSERT_EQUAL(std::string("true"), request.get("X-LOOL-WOPI-IsModifiedByUser"));

        passTest("Document uploaded on closing as expected.");
        return nullptr;
    }

    void onDocBrokerDestroy(const std::string& docKey) override
    {
        passTest("Document [" + docKey + "] uploaded and closed cleanly.");
    }

    /// The document is loaded.
    bool onDocumentLoaded(const std::string& message) override
    {
        LOG_TST("Doc (" << toString(_phase) << "): [" << message << ']');
        LOK_ASSERT_MESSAGE("Expected to be in Phase::WaitLoadStatus",
                           _phase == Phase::WaitLoadStatus);

        // Modify and wait for the notification.
        TRANSITION_STATE(_phase, Phase::WaitModifiedStatus);

        LOG_TST("Sending key input #" << ++_inputCount);
        WSD_CMD("key type=input char=97 key=0");
        WSD_CMD("key type=up char=0 key=512");

        return true;
    }

    /// The document is modified. Save, modify, and close it.
    bool onDocumentModified(const std::string& message) override
    {
        // We modify the document multiple times.
        // Only the first time is handled here.
        if (_phase == Phase::WaitModifiedStatus)
        {
            LOG_TST("Doc (" << toString(_phase) << "): [" << message << ']');
            LOK_ASSERT_MESSAGE("Expected to be in Phase::WaitModifiedStatus",
                               _phase == Phase::WaitModifiedStatus);

            // Save and immediately modify, then close the connection.
            WSD_CMD("save dontTerminateEdit=0 dontSaveIfUnmodified=0 "
                    "extendedData=CustomFlag%3DCustom%20Value%3BAnotherFlag%3DAnotherValue");

            LOG_TST("Sending key input #" << ++_inputCount);
            WSD_CMD("key type=input char=97 key=0");
            WSD_CMD("key type=up char=0 key=512");

            LOG_TST("Closing the connection.");
            deleteSocketAt(0);

            // Don't transition to WaitPutFile until after closing the socket.
            TRANSITION_STATE(_phase, Phase::WaitPutFile);
        }

        return true;
    }

    void invokeWSDTest() override
    {
        switch (_phase)
        {
            case Phase::Load:
            {
                TRANSITION_STATE(_phase, Phase::WaitLoadStatus);

                LOG_TST("Load: initWebsocket.");
                initWebsocket("/wopi/files/large-six-hundred.odt?access_token=anything");

                WSD_CMD("load url=" + getWopiSrc());
                break;
            }
            case Phase::WaitLoadStatus:
                break;
            case Phase::WaitModifiedStatus:
                break;
            case Phase::WaitPutFile:
                break;
        }
    }
};

UnitBase* unit_create_wsd(void) { return new UnitWOPISlow(); }

/* vim:set shiftwidth=4 softtabstop=4 expandtab: */
