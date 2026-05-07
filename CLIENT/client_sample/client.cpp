#include <SFML/Graphics.hpp>
#include <SFML/Network.hpp>
#include <iostream>
#include <unordered_map>
using namespace std;

#include "../../MMOSERVER_Termproject/MMOSERVER_Termproject/protocol_2026.h"

sf::TcpSocket socket;

constexpr auto SCREEN_WIDTH = 16;
constexpr auto SCREEN_HEIGHT = 16;
constexpr auto TILE_WIDTH = 65;
constexpr auto WINDOW_WIDTH = SCREEN_WIDTH * TILE_WIDTH;
constexpr auto WINDOW_HEIGHT = SCREEN_HEIGHT * TILE_WIDTH;
constexpr int BUF_SIZE = 1024;

int g_left_x;
int g_top_y;
int g_myid;

sf::RenderWindow* g_window;
sf::Font* g_font;

class OBJECT {
private:
    bool m_showing;
    sf::Sprite m_sprite;
    sf::Text m_name;
public:
    int m_x, m_y;
    char name[MAX_NAME_LEN];
    OBJECT(sf::Texture& t, int x, int y, int x2, int y2) {
        m_showing = false;
        m_sprite.setTexture(t);
        m_sprite.setTextureRect(sf::IntRect(x, y, x2, y2));
    }
    OBJECT() {
        m_showing = false;
    }
    void show() { m_showing = true; }
    void hide() { m_showing = false; }

    void a_move(int x, int y) { m_sprite.setPosition((float)x, (float)y); }
    void a_draw() { g_window->draw(m_sprite); }
    void move(int x, int y) { m_x = x; m_y = y; }

    void draw() {
        if (false == m_showing) return;

        if (m_x < g_left_x || m_x >= g_left_x + SCREEN_WIDTH ||
            m_y < g_top_y || m_y >= g_top_y + SCREEN_HEIGHT) return;

        float rx = (m_x - g_left_x) * 65.0f + 1;
        float ry = (m_y - g_top_y) * 65.0f + 1;
        m_sprite.setPosition(rx, ry);
        g_window->draw(m_sprite);
        auto size = m_name.getGlobalBounds();
        m_name.setPosition(rx + 32 - size.width / 2, ry - 10);
        g_window->draw(m_name);
    }

    void set_name(const char str[]) {
        m_name.setFont(*g_font);
        m_name.setString(str);
        m_name.setCharacterSize(20);
        m_name.setFillColor(sf::Color(255, 255, 0));
        m_name.setStyle(sf::Text::Bold);
    }
};

OBJECT avatar;
std::unordered_map <int, OBJECT> players;
OBJECT white_tile;
OBJECT black_tile;
std::string avatar_name;

sf::Texture* board;
sf::Texture* pieces;

void client_initialize()
{
    board = new sf::Texture;
    pieces = new sf::Texture;
    if (false == board->loadFromFile("chessmap.bmp")) {
        cout << "Texture Loading Error: chessmap.bmp\n";
        exit(-1);
    }
    if (false == pieces->loadFromFile("chess2.png")) {
        cout << "Texture Loading Error: chess2.png\n";
        exit(-1);
    }
    g_font = new sf::Font;
    if (false == g_font->loadFromFile("cour.ttf")) {
        cout << "Font Loading Error!\n";
        exit(-1);
    }
    white_tile = OBJECT{ *board, 5, 5, TILE_WIDTH, TILE_WIDTH };
    black_tile = OBJECT{ *board, 69, 5, TILE_WIDTH, TILE_WIDTH };
    avatar = OBJECT{ *pieces, 128, 0, 64, 64 };
    avatar.set_name(avatar_name.c_str());
    avatar.move(4, 4);
}

void client_finish()
{
    players.clear();
    delete g_font;
    delete board;
    delete pieces;
}

void send_packet(void* packet)
{
    unsigned char* p = reinterpret_cast<unsigned char*>(packet);
    size_t sent = 0;
    socket.send(packet, p[0], sent);
}

void ProcessPacket(char* ptr)
{
    switch (ptr[1])
    {
    case S2C_LOGIN_RESULT:
    {
        S2C_LoginResult* packet = reinterpret_cast<S2C_LoginResult*>(ptr);
        if (packet->success) {
            std::cout << "Login Success!\n";
            C2S_Login p;
            p.size = sizeof(p);
            p.type = C2S_LOGIN;
            strcpy_s(p.username, avatar_name.c_str());
            send_packet(&p);
        }
        else {
            std::cout << "Login Failed!\n";
            socket.disconnect();
        }
        break;
    }
    case S2C_AVATAR_INFO:
    {
        S2C_AvatarInfo* packet = reinterpret_cast<S2C_AvatarInfo*>(ptr);
        g_myid = packet->playerId;
        avatar.m_x = packet->x;
        avatar.m_y = packet->y;

        g_left_x = packet->x - 8;
        g_top_y = packet->y - 8;
        avatar.show();
    }
    break;

    case S2C_ADD_OBJECT:
    {
        S2C_AddObject* my_packet = reinterpret_cast<S2C_AddObject*>(ptr);
        int id = my_packet->object_id;
        players[id] = OBJECT{ *pieces, 0, 0, 64, 64 };
        players[id].move(my_packet->x, my_packet->y);
        players[id].set_name(my_packet->obj_name);
        players[id].show();
        break;
    }
    case S2C_MOVE_OBJECT:
    {
        S2C_MoveObject* my_packet = reinterpret_cast<S2C_MoveObject*>(ptr);
        int other_id = my_packet->object_id;
        if (other_id == g_myid) {
            avatar.move(my_packet->x, my_packet->y);
            g_left_x = my_packet->x - 8;
            g_top_y = my_packet->y - 8;
        }
        else {
            players[other_id].move(my_packet->x, my_packet->y);
        }
        break;
    }

    case S2C_REMOVE_OBJECT:
    {
        S2C_RemoveObject* my_packet = reinterpret_cast<S2C_RemoveObject*>(ptr);
        int other_id = my_packet->object_id;
        if (other_id == g_myid)
            avatar.hide();
        else
            players.erase(other_id);
        break;
    }
    }
}

void process_data(char* net_buf, size_t io_byte)
{
    char* ptr = net_buf;
    static size_t in_packet_size = 0;
    static size_t saved_packet_size = 0;
    static char packet_buffer[BUF_SIZE];

    while (0 != io_byte) {
        if (0 == in_packet_size) in_packet_size = ptr[0];
        if (io_byte + saved_packet_size >= in_packet_size) {
            memcpy(packet_buffer + saved_packet_size, ptr, in_packet_size - saved_packet_size);
            ProcessPacket(packet_buffer);
            ptr += in_packet_size - saved_packet_size;
            io_byte -= in_packet_size - saved_packet_size;
            in_packet_size = 0;
            saved_packet_size = 0;
        }
        else {
            memcpy(packet_buffer + saved_packet_size, ptr, io_byte);
            saved_packet_size += io_byte;
            io_byte = 0;
        }
    }
}

void client_main()
{
    char net_buf[BUF_SIZE];
    size_t	received;

    auto recv_result = socket.receive(net_buf, BUF_SIZE, received);
    if (recv_result == sf::Socket::Error || recv_result == sf::Socket::Disconnected) exit(-1);

    if (recv_result != sf::Socket::NotReady)
        if (received > 0) process_data(net_buf, received);

    for (int i = 0; i < SCREEN_WIDTH; ++i) {
        for (int j = 0; j < SCREEN_HEIGHT; ++j)
        {
            int tile_x = i + g_left_x;
            int tile_y = j + g_top_y;

            if ((tile_x < 0) || (tile_y < 0) || (tile_x >= WORLD_WIDTH) || (tile_y >= WORLD_HEIGHT)) continue;

            if (0 == (tile_x / 3 + tile_y / 3) % 2) {
                white_tile.a_move(TILE_WIDTH * i, TILE_WIDTH * j);
                white_tile.a_draw();
            }
            else {
                black_tile.a_move(TILE_WIDTH * i, TILE_WIDTH * j);
                black_tile.a_draw();
            }
        }
    }

    avatar.draw();
    for (auto& pl : players) pl.second.draw();

    sf::Text text;
    text.setFont(*g_font);
    char buf[100];
    sprintf_s(buf, "(%d, %d)", avatar.m_x, avatar.m_y);
    text.setString(buf);
    text.setFillColor(sf::Color::White);
    text.setOutlineColor(sf::Color::Black);
    text.setOutlineThickness(2.0f);
    g_window->draw(text);
}

int main()
{
    wcout.imbue(locale("korean"));
    std::cout << "Enter User Name : ";
    std::cin >> avatar_name;
    std::string server_ip;
    std::cout << "Enter Server IP (default: 127.0.0.1) : ";
    std::cin.ignore();
    std::getline(std::cin, server_ip);
    if (server_ip.empty()) server_ip = "127.0.0.1";
    sf::Socket::Status status = socket.connect(server_ip, PORT);
    socket.setBlocking(false);

    if (status != sf::Socket::Done) while (true);

    client_initialize();

    sf::RenderWindow window(sf::VideoMode(WINDOW_WIDTH, WINDOW_HEIGHT), "2D CLIENT - 16x16 View");
    g_window = &window;

    while (window.isOpen())
    {
        sf::Event event;
        while (window.pollEvent(event))
        {
            if (event.type == sf::Event::Closed) window.close();
            if (event.type == sf::Event::KeyPressed) {
                short target_x = avatar.m_x;
                short target_y = avatar.m_y;
                bool moved = false;

                switch (event.key.code) {
                case sf::Keyboard::Left:  target_x -= 1; moved = true; break;
                case sf::Keyboard::Right: target_x += 1; moved = true; break;
                case sf::Keyboard::Up:    target_y -= 1; moved = true; break;
                case sf::Keyboard::Down:  target_y += 1; moved = true; break;
                case sf::Keyboard::Escape: window.close(); break;
                }
                
                if (moved) {
                    C2S_Move p;
                    p.size = sizeof(p);
                    p.type = C2S_MOVE;
                    p.x = target_x;
                    p.y = target_y;
                    p.move_time = 0; // 아직 서버에 타이머가 없으므로 0
                    send_packet(&p);
                }
            }
        }

        window.clear();
        client_main();
        window.display();
    }
    client_finish();

    return 0;
}