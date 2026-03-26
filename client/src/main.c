#include "raylib.h"

int main(void)
{
    // 初始化窗口
    const int screenWidth = 800;
    const int screenHeight = 600;

    InitWindow(screenWidth, screenHeight, "Raylib 示例 - Hello World");

    // 设置目标帧率
    SetTargetFPS(60);

    // 主循环
    while (!WindowShouldClose())
    {
        // 开始绘制
        BeginDrawing();

        // 清空背景为白色
        ClearBackground(RAYWHITE);

        // 绘制文本
        DrawText("恭喜！Raylib 运行成功!", 190, 200, 20, LIGHTGRAY);
        DrawText("按 ESC 退出", 320, 280, 20, GRAY);

        // 绘制一个旋转的矩形
        static float rotation = 0.0f;
        rotation += 1.0f;
        if (rotation >= 360.0f)
        {
            rotation = 0.0f;
        }

        DrawRectanglePro(
            (Rectangle){ screenWidth / 2.0f, screenHeight / 2.0f + 100, 100, 100 },
            (Vector2){ 50, 50 },
            rotation,
            MAROON
        );

        // 结束绘制
        EndDrawing();
    }

    // 关闭窗口
    CloseWindow();

    return 0;
}
