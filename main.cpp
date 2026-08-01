/*
* Copyright (C) 2026 Ryan83602
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published
 * by the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */
#include <iostream>
#include <string>
#include <vector>
#include <map>
#include <regex>
#include <sstream>
#include <filesystem>
#include <fstream>
#include <thread>
#include <future>
#include <chrono>
#include <algorithm>
#include <cctype>
#include <mutex>
#include <clocale>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

using namespace std;
using json = nlohmann::json;
namespace fs = std::filesystem;

#ifdef _WIN32
#include <windows.h>
static void enable_utf8_console() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IOFBF, 4096);
    setvbuf(stderr, nullptr, _IOFBF, 4096);
}
#else
static void enable_utf8_console() {}
#endif

#define COLOR_RESET   "\033[0m"
#define COLOR_RED     "\033[31m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_WHITE   "\033[37m"

static mutex log_mutex;
static ofstream log_file;

void init_log_file() {
    log_file.open("log.txt", ios::app);
}

void close_log_file() {
    if (log_file.is_open()) {
        log_file.flush();
        log_file.close();
    }
}

static void write_log(const string& level, const string& msg, const string& color) {
    lock_guard<mutex> lock(log_mutex);
    if (level == "INFO") {
        cout << color << "[INFO] " << msg << COLOR_RESET << endl;
    } else if (level == "WARN") {
        cout << color << "[WARN] " << msg << COLOR_RESET << endl;
    } else if (level == "ERR") {
        cerr << color << "[ERR] " << msg << COLOR_RESET << endl;
    }
    if (log_file.is_open()) {
        log_file << "[" << level << "] " << msg << endl;
        log_file.flush();
    }
}

static void log_info(const string& msg) { write_log("INFO", msg, COLOR_WHITE); }
static void log_warn(const string& msg) { write_log("WARN", msg, COLOR_YELLOW); }
static void log_err(const string& msg) { write_log("ERR", msg, COLOR_RED); }

const string HEADERS_API =
    "User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36\r\n"
    "Referer: https://comic.hypergryph.com/\r\n"
    "Accept: application/json, text/plain, */*\r\n";

const string HEADERS_IMG =
    "User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36\r\n"
    "Referer: https://comic.hypergryph.com/\r\n";

string g_working_template = "";

string sanitize_path(const string& name) {
    string result;
    for (unsigned char ch : name) {

        if (ch < 0x20 || ch == 0x7F) {
            continue;
        }

        if (ch == '\\' || ch == '/' || ch == ':' || ch == '*' || ch == '?' ||
            ch == '"' || ch == '<' || ch == '>' || ch == '|') {
            continue;
        }
        result.push_back(static_cast<char>(ch));
    }

    size_t start = result.find_first_not_of(" \t\n\r");
    if (start == string::npos) return "chapter";
    size_t end = result.find_last_not_of(" \t\n\r");
    result = result.substr(start, end - start + 1);
    if (result.empty()) return "chapter";
    return result;
}

void find_all_image_urls(const json& data, vector<string>& urls) {
    if (data.is_string()) {
        string s = data.get<string>();
        if (s.rfind("http://", 0) == 0 || s.rfind("https://", 0) == 0 || s.rfind("//", 0) == 0) {
            string lower = s;
            transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
            if (lower.find(".jpg") != string::npos || lower.find(".jpeg") != string::npos ||
                lower.find(".png") != string::npos || lower.find(".webp") != string::npos ||
                lower.find(".gif") != string::npos ||
                lower.find("/images/") != string::npos || lower.find("/comic/") != string::npos) {
                if (s.rfind("//", 0) == 0) s = "https:" + s;
                urls.push_back(s);
            }
        }
    } else if (data.is_array()) {
        for (const auto& item : data) {
            find_all_image_urls(item, urls);
        }
    } else if (data.is_object()) {
        for (auto& [key, val] : data.items()) {
            find_all_image_urls(val, urls);
        }
    }
}

static size_t write_callback(void* contents, size_t size, size_t nmemb, string* out) {
    size_t total = size * nmemb;
    out->append(static_cast<char*>(contents), total);
    return total;
}


string http_get(const string& url, const string& headers, long timeout_sec = 10) {
    CURL* curl = curl_easy_init();
    if (!curl) return "";
    string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_sec);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    struct curl_slist* list = nullptr;
    istringstream hs(headers);
    string line;
    while (getline(hs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) {
            list = curl_slist_append(list, line.c_str());
        }
    }
    if (list) {
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
    }

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        log_err("CURL error: " + string(curl_easy_strerror(res)));
        response.clear();
    }
    curl_slist_free_all(list);
    curl_easy_cleanup(curl);
    return response;
}

json get_comic_data(const string& comic_id) {
    string url = "https://comic.hypergryph.com/api/comic/" + comic_id;
    string resp = http_get(url, HEADERS_API, 10);
    if (resp.empty()) return json();
    try {
        json j = json::parse(resp);
        if (j.contains("code") && j["code"] == 0) {
            return j;
        }
    } catch (...) {}
    return json();
}

pair<int, string> get_page_image_url(const string& comic_id, const string& ep_id, int page_num) {
    string url = "https://comic.hypergryph.com/api/comic/" + comic_id +
                 "/episode/" + ep_id + "/page?pageNum=" + to_string(page_num);
    int max_retries = 5;
    for (int attempt = 1; attempt <= max_retries; ++attempt) {
        string resp = http_get(url, HEADERS_API, 15);
        if (!resp.empty()) {
            try {
                json j = json::parse(resp);
                if (j.contains("code") && j["code"] == 0 && j.contains("data")) {
                    vector<string> urls;
                    find_all_image_urls(j["data"], urls);
                    if (!urls.empty()) {
                        return {page_num, urls[0]};
                    }
                }
            } catch (...) {}
        }
        log_warn("Cannot get real URL for page " + to_string(page_num) +
                 ", retry " + to_string(attempt) + "/" + to_string(max_retries));
        this_thread::sleep_for(chrono::seconds(1));
    }
    log_err("Fatal Error: Can not get real url for page " + to_string(page_num));
    return {page_num, ""};
}

bool download_image(const string& url, const string& save_path) {
    if (fs::exists(save_path) && fs::file_size(save_path) > 0) {
        log_warn("File \"" + save_path + "\" already exists, skipped.");
        return true;
    }
    int max_retries = 5;
    for (int attempt = 1; attempt <= max_retries; ++attempt) {
        string data = http_get(url, HEADERS_IMG, 25);
        if (!data.empty()) {
            ofstream file(save_path, ios::binary);
            if (file) {
                file.write(data.data(), data.size());
                file.close();
                log_info("File \"" + save_path + "\" downloaded successfully.");
                return true;
            }
        }
        log_warn("Download failed for \"" + save_path + "\", retry " +
                 to_string(attempt) + "/" + to_string(max_retries));
        this_thread::sleep_for(chrono::seconds(1));
    }
    log_err("Fatal Error: Can not download image: " + save_path);
    return false;
}

void download_episode(const string& comic_id,
                      const json& episode,
                      const fs::path& save_root,
                      int idx,
                      int total_eps) {
    string ep_id;
    if (episode.contains("cid")) ep_id = episode["cid"].get<string>();
    else if (episode.contains("id")) ep_id = episode["id"].get<string>();
    else return;

    string ep_title = sanitize_path(episode.value("title", "未命名章节"));
    string short_title = sanitize_path(episode.value("shortTitle", ""));

    string base_name = short_title.empty() ? ep_title : "[" + short_title + "] " + ep_title;
    int width = max(3, static_cast<int>(to_string(total_eps).size()));
    string prefix = to_string(idx);
    while (prefix.size() < width) prefix = "0" + prefix;
    string dir_name = prefix + "_" + base_name;

    fs::path save_dir;
    try {
        save_dir = save_root / fs::u8path(dir_name);
        fs::create_directories(save_dir);
    } catch (const fs::filesystem_error& e) {
        log_warn("Cannot create directory with name: " + dir_name + ", using fallback: " + prefix + "_chapter");
        string fallback = prefix + "_chapter";
        save_dir = save_root / fs::u8path(fallback);
        fs::create_directories(save_dir);
    } catch (...) {
        log_err("Unexpected error, using emergency fallback: " + prefix + "_ep");
        save_dir = save_root / fs::u8path(prefix + "_ep");
        fs::create_directories(save_dir);
    }

    json res_data;
    if (!g_working_template.empty()) {
        string url = g_working_template;
        size_t pos1 = url.find("{comic_id}");
        if (pos1 != string::npos) url.replace(pos1, 10, comic_id);
        size_t pos2 = url.find("{ep_id}");
        if (pos2 != string::npos) url.replace(pos2, 7, ep_id);
        string resp = http_get(url, HEADERS_API, 10);
        if (!resp.empty()) {
            try {
                json j = json::parse(resp);
                if (j.contains("code") && j["code"] == 0) res_data = j;
            } catch (...) {}
        }
    } else {
        vector<string> candidates = {
            "https://comic.hypergryph.com/api/comic/episode?id={ep_id}",
            "https://comic.hypergryph.com/api/comic/episode/{ep_id}",
            "https://comic.hypergryph.com/api/episode/{ep_id}",
            "https://comic.hypergryph.com/api/comic/{comic_id}/episode/{ep_id}"
        };
        for (const auto& tmpl : candidates) {
            string url = tmpl;
            size_t pos1 = url.find("{comic_id}");
            if (pos1 != string::npos) url.replace(pos1, 10, comic_id);
            size_t pos2 = url.find("{ep_id}");
            if (pos2 != string::npos) url.replace(pos2, 7, ep_id);
            string resp = http_get(url, HEADERS_API, 4);
            if (!resp.empty()) {
                try {
                    json j = json::parse(resp);
                    if (j.contains("code") && j["code"] == 0) {
                        g_working_template = tmpl;
                        res_data = j;
                        break;
                    }
                } catch (...) {}
            }
        }
    }

    if (res_data.empty()) {
        log_err("Failed to get episode structure for \"" + dir_name + "\", skipped.");
        return;
    }

    auto page_infos = res_data["data"]["pageInfos"];
    int page_count = page_infos.is_array() ? page_infos.size() : 0;
    if (page_count == 0) {
        log_warn("Episode \"" + dir_name + "\" has 0 pages, skipped.");
        return;
    }


    if (fs::exists(save_dir)) {
        vector<string> downloaded;
        for (const auto& entry : fs::directory_iterator(save_dir)) {
            if (fs::is_regular_file(entry)) {
                string ext = entry.path().extension().string();
                transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                if (ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".webp" || ext == ".gif") {
                    if (fs::file_size(entry) > 0) {
                        downloaded.push_back(entry.path().filename().string());
                    }
                }
            }
        }
        if (downloaded.size() >= static_cast<size_t>(page_count)) {
            log_warn("Episode \"" + dir_name + "\" already complete (" +
                     to_string(page_count) + " pages), skipped.");
            return;
        }
    }

    log_info("Downloading episode: " + dir_name + " (" + to_string(page_count) + " pages)...");

    vector<future<pair<int, string>>> url_futures;
    for (int p = 1; p <= page_count; ++p) {
        url_futures.emplace_back(async(launch::async, get_page_image_url,
                                       comic_id, ep_id, p));
    }
    map<int, string> img_urls;
    for (auto& fut : url_futures) {
        auto [page_num, url] = fut.get();
        if (!url.empty()) {
            img_urls[page_num] = url;
        }
    }

    if (img_urls.size() < static_cast<size_t>(page_count)) {
        log_warn("Only got " + to_string(img_urls.size()) + " out of " +
                 to_string(page_count) + " image links for episode \"" + dir_name + "\".");
    }

    vector<future<bool>> dl_futures;
    for (const auto& [page_num, url] : img_urls) {
        string ext = fs::path(url).extension().string();
        if (ext.empty()) ext = ".jpg";
        size_t q = ext.find('?');
        if (q != string::npos) ext = ext.substr(0, q);
        string img_name = to_string(page_num);
        while (img_name.size() < 3) img_name = "0" + img_name;
        img_name += ext;
        fs::path img_path = save_dir / fs::u8path(img_name);
        dl_futures.emplace_back(async(launch::async, download_image, url, img_path.string()));
    }
    for (auto& fut : dl_futures) {
        fut.get();
    }

    this_thread::sleep_for(chrono::milliseconds(50));
}

int main(int argc, char* argv[]) {
    setlocale(LC_ALL, "en_US.UTF-8");
    enable_utf8_console();
    init_log_file();

    curl_global_init(CURL_GLOBAL_ALL);

    string comic_id = "6253";
    if (argc > 1) {
        comic_id = argv[1];
    }

    json res = get_comic_data(comic_id);
    if (res.empty()) {
        log_err("Cannot connect to Hypergryph server.");
        curl_global_cleanup();
        close_log_file();
        return 1;
    }

    auto comic_data = res["data"];
    string comic_title = sanitize_path(comic_data.value("title", "Comic_" + comic_id));

    fs::path exe_dir;
    try {
        exe_dir = fs::u8path(argv[0]).parent_path();
    } catch (...) {
        exe_dir = ".";
    }

    fs::path save_root;
    try {
        save_root = exe_dir / fs::u8path(comic_title);
    } catch (...) {
        save_root = exe_dir / "Comic";
    }

    auto episodes = comic_data["episodes"];
    if (!episodes.is_array() || episodes.empty()) {
        log_warn("No downloadable episodes found.");
        curl_global_cleanup();
        close_log_file();
        return 1;
    }

    vector<json> ep_list;
    for (auto& ep : episodes) ep_list.push_back(ep);
    reverse(ep_list.begin(), ep_list.end());
    int total_eps = ep_list.size();

    log_info("Connected successfully! Comic: \"" + comic_title + "\"");
    log_info("Total episodes: " + to_string(total_eps) + ", starting synchronization...");
    log_info("Download location: " + save_root.string());

    int idx = 1;
    for (const auto& ep : ep_list) {
        log_info("Processing episode " + to_string(idx) + "/" + to_string(total_eps));
        download_episode(comic_id, ep, save_root, idx, total_eps);
        ++idx;
    }

    log_info("All " + to_string(total_eps) + " episodes synchronized. Check folder: \"" + save_root.string() + "\"");
    curl_global_cleanup();
    close_log_file();
    return 0;
}