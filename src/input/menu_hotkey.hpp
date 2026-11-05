#pragma once
namespace litedx12
{
// Window messages and polling share the edge, so repeats cannot toggle twice.
struct MenuHotkey
{
    bool down{};
    bool Observe(bool pressed)
    {
        const bool released = down && !pressed;
        down = pressed;
        return released;
    }
};
}
