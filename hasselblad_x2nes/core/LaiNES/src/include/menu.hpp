#pragma once
#include <functional>
#include <SDL2/SDL.h>
#include <string>
#include <vector>
#include "gui.hpp"

namespace GUI {


class Entry
{
    std::string label;
    std::function<void()> callback;
    std::function<bool()> enabled_check;

    bool selected = false;
    SDL_Texture* whiteTexture = nullptr;
    SDL_Texture* redTexture   = nullptr;
    SDL_Texture* greyTexture  = nullptr;

  public:
    Entry(std::string label, std::function<void()> callback = []{}, std::function<bool()> enabled_check = []{ return true; });
    ~Entry();

    void set_label(std::string label);
    inline std::string& get_label() { return label; }
    inline bool is_enabled() { return enabled_check(); }

    virtual void select()   { selected = true;  };
    virtual void unselect() { selected = false; };
    void trigger() { callback(); };
    virtual void render(int x, int y);
};

class ControlEntry : public Entry
{
    SDL_Scancode* key;
    int* button;
    Entry* keyEntry;

  public:
    ControlEntry(std::string action, SDL_Scancode* key);
    ControlEntry(std::string action, int* button);
    void select()   { Entry::select();   keyEntry->select();   }
    void unselect() { Entry::unselect(); keyEntry->unselect(); }
    void render(int x, int y) { Entry::render(x, y); keyEntry->render(TEXT_RIGHT, y); }
};

class CycleEntry : public Entry
{
    int* value;
    int min_val;
    int max_val;
    std::string prefix;
    std::string suffix;
    std::function<void(int)> on_change;

    void cycle();
    void update_label();

  public:
    CycleEntry(std::string prefix, int* value, int min_val, int max_val,
               std::function<void(int)> on_change, std::string suffix = "x",
               std::function<bool()> enabled_check = []{ return true; });
};

class ToggleEntry : public Entry
{
    bool* value;
    std::string prefix;
    std::function<void(bool)> on_change;

    void toggle();
    void update_label();

  public:
    ToggleEntry(std::string prefix, bool* value, std::function<void(bool)> on_change,
                std::function<bool()> enabled_check = []{ return true; });
};

class Menu
{
  protected:
    const int MAX_ENTRY = GUI::HEIGHT / FONT_SZ - 2;
    int cursor = 0;
    int top = 0;
    int bottom = MAX_ENTRY;

  public:
    std::vector<Entry*> entries;
    Entry* errorMessage = nullptr;

    void add(Entry* entry);
    void clear();
    void clear_error();
    void sort_by_label();
    virtual void update(u8 const* keys);
    void render();

  protected:
    void jump_to(int newCursor);
};

class FileMenu : public Menu
{
    void change_dir(std::string dir);
    void load_rom(std::string path);

  public:
    FileMenu();
    void update(u8 const* keys) override;
};


}
