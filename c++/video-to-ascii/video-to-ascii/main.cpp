//
//  main.cpp
//  video-to-ascii
//
//  Created by Wang Zerui on 1/6/21.
//

#include "opencv2/opencv.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <limits.h>
#include <stdlib.h>
#include <csignal>

// Utilities
#include "colorUtil.hpp"
#include "cmdutils.hpp"

// Structs
#include "Structs.h"

// Finding terminal size and other Windows specifics
#if defined(_WIN64) || defined(_WIN32)
#include <windows.h>
#define PATH_MAX MAX_PATH
#define sleep(x) Sleep(x * 1000)
#define clear() system("cls")
#else
#include <sys/ioctl.h> //ioctl() and TIOCGWINSZ
#include <unistd.h> // for STDOUT_FILENO
#define clear() system("clear && printf '\e[3J'")
#endif


// Namespaces
using namespace std;
using namespace cv;
using namespace std::chrono;

// MARK: Definitions
#define MAX_SYNC_OFFSET 10s // Do not attempt to A/V sync if error is too large

// MARK: Constants
const string DENSITY[] = {
    " ", " ", ".", ":", "!", "+", "*", "e", "$", "@", "8",
    ".", "*", "e", "s", "◍",
    "░", "▒", "▓", "█"
};

// MARK: Globals
#if !defined(_WIN64) && !defined(_WIN32)
struct winsize termSize;
#endif
vector<string> buffer;
bool lock_buff = false;
bool halt_loop = false;
uint16_t scn_col = 80;
uint16_t scn_row = 45;
uint32_t skippedFrames = 0;

// MARK:- Renderer function, running async

void decToAscii(VideoCapture cap) {
    lock_buff = true;
    while (!halt_loop) {
        Mat frame;
        // Capture frame-by-frame
        cap >> frame;

        // If the frame is empty, break immediately
        if (frame.empty()) break;
        
        // Resize frame to the size of the terminal
        resize(frame, frame, Size(scn_col, scn_row), 0, 0, INTER_AREA);
        
        uint8_t* pixelPtr = (uint8_t*)frame.data;
        int cn = frame.channels();
        Scalar_<uint8_t> bgrPixel;
        
        if (halt_loop) return;
        buffer.push_back("");
        buffer[buffer.size()-1] += "\u001b[" + to_string(scn_col) + "D\u001b[" + to_string(scn_row) + "A";
        for (int i = 0; i < frame.rows; i++) {
            for (int j = 0; j < frame.cols; j++) {
                bgrPixel.val[2] = pixelPtr[i * frame.cols * cn + j * cn + 0]; // B
                bgrPixel.val[1] = pixelPtr[i * frame.cols * cn + j * cn + 1]; // G
                bgrPixel.val[0] = pixelPtr[i * frame.cols * cn + j * cn + 2]; // R

                // do something with RGB values...
                const uint8_t intensity = (bgrPixel[0] + bgrPixel[1] + bgrPixel[2]) / 40.26;
                buffer[buffer.size()-1] += "\u001b[38;5;" + to_string(getColorId(bgrPixel[0], bgrPixel[1], bgrPixel[2])) + "m" + DENSITY[intensity];
            }
            if (i != frame.rows - 1) buffer[buffer.size()-1] += "\n";
        }
        buffer.shrink_to_fit();
    }
    lock_buff = false;
}

// MARK:- Termination Handler

void cleanUp(int signum, bool clear = false) {
    
    halt_loop = true;
    
    // Restore cursor
#if !defined(_WIN64) && !defined(_WIN32)
    system("tput cvvis");
#endif
    
    // Clear terminal
    if (clear) clear();

    cout << "\u001b[0m"; // Reset color
    if (skippedFrames != 0) writeMsg("Skipped " + to_string(skippedFrames) + " frame(s)", LOG_WARN);
    writeMsg("Goodbye!", LOG_INFO);
    
    if (signum != 0) writeMsg("Terminating with exit code " + to_string(signum), LOG_ERROR); // Non-0 exit = error
    
    exit(signum);
}

void sigIntHandler(int signum) {
    halt_loop = true;
    
    // Clear terminal
    clear();
    writeMsg("Received signal " + to_string(signum) + ", halting", LOG_WARN);
    cleanUp(signum);
}

// MARK:- Error handlers

int handleCV2Error( int status, const char* func_name,
            const char* err_msg, const char* file_name,
            int line, void* userdata )
{
    //Do nothing -- will suppress console output
    return 0;   //Return value is not used
}

// MARK:- Argument actions

FlagActions fActs[] = {
    {"i", "Prints out infomation about this program", true, writeBanner},
    {"width", "Override width of viewport (in characters)", false, NULL},
    {"height", "Override height of viewport (in characters)", false, NULL}
};

// MARK:- Main

int main(int argc, char** argv) {
    // Fast IO speed
    cout.tie(0);
    ios_base::sync_with_stdio(0);

#if defined(_WIN64) || defined(_WIN32)
    //_setmode(_fileno(stdout), _O_U16TEXT);
    system("chcp 65001 >> NUL"); // Enable cmd.exe unicode mode
#endif

    // Register SIGINT signal handler
    signal(SIGINT, sigIntHandler);
    
    // Get terminal size
#if defined(_WIN64) || defined(_WIN32)
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi);
    scn_col = csbi.srWindow.Right - csbi.srWindow.Left + 1;
    scn_row = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
#else
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &termSize); // This only works on Unix
    scn_col = termSize.ws_col;
    scn_row = termSize.ws_row;
#endif
    
    // MARK: Parse command line flags
    const vector<FlagOps> cmdFlags = parseArgs(argc, argv, fActs, 3);
    
    // Handle option flags
    for (uint16_t i = 0; i < cmdFlags.size(); i++) {
        // cout << "Flag: " << cmdFlags[i].flag << ", value: " << cmdFlags[i].val;
        const auto f = cmdFlags[i];
        if (f.flag == "width") scn_col = std::stoi(f.val);
        else if (f.flag == "height") scn_row = std::stoi(f.val);
    }
    
    if (argc < 2) {
        writeMsg("No video path provided. Run with the -h flag to view help", LOG_FATAL);
        cleanUp(-1);
    }
    
    // MARK: Retrieve absolute path from user-entered relative path
    char pathBuff[PATH_MAX];
#if defined(_WIN64) || defined(_WIN32)
    _fullpath(pathBuff, argv[argc - 1], PATH_MAX);
    char* absPath = pathBuff;
#else
    char* absPath = realpath(argv[argc - 1], pathBuff);
#endif
    if (!absPath) {
        writeMsg("Failed to determine absolute video path", LOG_FATAL);
        cleanUp(-1);
    }
    
    // MARK: Fork parent process
#if !defined(_WIN64) && !defined(_WIN32)
    pid_t pid = fork();

    if (pid == -1) {
        // Error, failed to fork()
        writeMsg("Failed to fork parent process", LOG_FATAL);
        cleanUp(123);
    }
    // MARK:- Parent thread
    else if (pid > 0) {
#endif
        // Redirect CV2 errors
        // redirectError(handleCV2Error);
        VideoCapture cap(absPath);
        
        // Check if camera was opened successfully
        if (!cap.isOpened()) {
            writeMsg("Error opening video file, check if the file exists and is readable", LOG_FATAL);
            cleanUp(-1);
        }
        
        // Calculate target time spent on one frame
        const auto targetDelay = 1000ms / cap.get(CAP_PROP_FPS);
        
        thread decThread(decToAscii, cap); // Start renderer thread
        
        // MARK: Write banner and loading text to console
        writeBanner();
        writeMsg("Performing initial buffering, please wait a second...", LOG_INFO);
        sleep(1);
        
        // thread audioThread(playAudio, HOME + vidPath); // Start audio thread
        
        unsigned int i = 0;
        const auto t1 = high_resolution_clock::now();
        // chrono::duration<double, milli> syncTime = 0ms;
        
        // Hide cursor
        #if defined(_WIN64) || defined(_WIN32)
        HANDLE hStdOut = NULL;
        CONSOLE_CURSOR_INFO curInfo;

        hStdOut = GetStdHandle(STD_OUTPUT_HANDLE);
        GetConsoleCursorInfo(hStdOut, &curInfo);
        curInfo.bVisible = FALSE;
        SetConsoleCursorInfo(hStdOut, &curInfo);
        #else
        system("tput civis");
        #endif
        // Clear terminal
        clear();

        // MARK:- Main display loop
        while (!halt_loop) {
            if (i >= buffer.size()) break;

            fputs(buffer[i].c_str(), stdout);
            // buffer[i] = "";
            
            i++;
            
            const auto t2 = high_resolution_clock::now();
            const auto dur = (targetDelay * (i + 1)) - (t2 - t1);
                        
            if (dur >= 0ms) this_thread::sleep_for(dur);
            else if ((-dur) >= MAX_SYNC_OFFSET) {
                writeMsg("A/V sync error: error is too large, not syncing", LOG_FATAL);
                cleanUp(256);
            }
            else if ((-dur) > targetDelay) {
                const uint8_t skipFrames = (-dur) / targetDelay;
                i += skipFrames;
                skippedFrames += skipFrames;
            }
        }
        writeMsg("End of video, thanks for watching!", LOG_INFO);
        
        decThread.detach(); // If the thread is still running, detach it (at this point it should not be running)
        
        // Release the video capture object
        cap.release();
        
        cleanUp(0, true);
#if !defined(_WIN64) && !defined(_WIN32)
    }
    // MARK:- Child thread
    else {
        sleep(1); // Ensure playback starts at the same time
        
        string path(absPath);
        char *args[] = {
            (char*)"ffplay",
            (char*)"-vn",
            (char*)"-nodisp",
            (char*)path.c_str(),
            NULL
        };
        
        // Redirect stderr to /dev/null
        const int fd = open("/dev/null", O_WRONLY | O_CREAT, 0666);
        dup2(fd, 2); // Change stderr to opened file
        dup2(fd, 1);
        #if defined(_WIN64) || defined(_WIN32)
        system((string("C:\ffmpeg\bin\ffplay\ffplay.exe -vn -nodisp ") + string(absPath)).c_str());
        #else
        execve("/usr/local/bin/ffplay", args, {});
        #endif
        close(fd); // Close file (although this will never happen)
        _exit(EXIT_FAILURE);   // exec never returns
    }
#endif
    
    return 0;
}
