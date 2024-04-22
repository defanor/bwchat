/**
   @file bwchat.js
   @brief A client-side script for use with bwchat
   @author defanor <defanor@thunix.net>
   @date 2024
   @copyright MIT license
*/

var mediaRecorder = null;
var streamButton = null;

function handleDataAvailable(event) {
    if (event.data.size > 0) {
        const formData  = new FormData();
        formData.append("stream", "");
        formData.append("nick", document.getElementsByName("nick")[0].value);
        formData.append("message", event.data);
        // TODO: would be better to run a timer once the request is
        // processed, rather than to issue them regularly.
        fetch("chat", { method: "POST", body: formData })
            .catch((err) => {
                console.error(err);
                mediaRecorder.stop();
                mediaRecorder = null;
                streamButton.value = "Start streaming";
            });
    }
}

function stream() {
    if (streamButton.value == "Start streaming") {
        navigator.mediaDevices
            .getUserMedia({audio: true, video: false})
            .then((mediaStream) => {
                const options = { mimeType: "audio/ogg; codec=opus" };
                mediaRecorder = new MediaRecorder(mediaStream, options);
                mediaRecorder.ondataavailable = handleDataAvailable;
                mediaRecorder.start(500);
                streamButton.value = "Stop streaming";
            })
            .catch((err) => console.error(err));
    } else {
        mediaRecorder.stop();
        mediaRecorder = null;
        streamButton.value = "Start streaming";
    }
}

addEventListener("DOMContentLoaded", (event) => {
    // Set the streaming button
    streamButton = document.createElement("input");
    streamButton.type = "button";
    streamButton.value = "Start streaming";
    streamButton.onclick = stream;
    document.body.appendChild(streamButton);

    // Setup AJAX-based message submission
    var messages = document.getElementById("messages");
    var chatInputForm = document.getElementById("chatInputForm");
    chatInputForm.addEventListener("submit", function (e) {
        var nick = document.getElementsByName("nick")[0];
        var message = document.getElementsByName("message")[0];
        if (nick.value.length > 0 && message.value.length > 0) {
            const formData  = new FormData();
            formData.append("nick", nick.value);
            formData.append("message", message.value);
            fetch("chat", { method: "POST", body: formData })
                .catch((err) => console.error(err));
            message.value = '';
            e.preventDefault();
            return false;
        }
    });

    // Setup AJAX-based message retrieval
    fetch("messages").then((response) => {
        const reader = response.body.getReader();
        reader.read().then(function pump({done, value}) {
            if (done) {
                return;
            }
            var str = new TextDecoder().decode(value);
            if (str.trim().length > 0) {
                messages.innerHTML += str;
                // if (messages.lastElementChild.lastElementChild
                //     .tagName == "AUDIO") {
                //     messages.lastElementChild.lastElementChild.play();
                // }
            }
            while (messages.childElementCount > 20) {
                messages.firstElementChild.remove();
            }
            reader.read().then(pump);
        });
    }).catch((err) => console.error(err));
});
