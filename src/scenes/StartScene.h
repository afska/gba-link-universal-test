#ifndef START_SCENE_H
#define START_SCENE_H

#include "Scene.h"

#include "../objects/Horse.h"

class StartScene : public Scene {
 public:
  StartScene(const GBFS_FILE* _fs);

  void init() override;
  void update() override;

 private:
  bn::optional<bn::regular_bg_ptr> background;
  bn::unique_ptr<Horse> horse;
  bn::vector<bn::sprite_ptr, 64> textSprites;
  bn::sprite_text_generator textGenerator;
  bn::sprite_text_generator textGeneratorAccent;
  bn::fixed videoFrame = 0;
  int lastBeat = 0;
  bn::fixed extraSpeed = 0;
  bool isConnected = false;

  void onConnected();
  void onDisconnected();
  void updateVideo();
  void print(bn::string<128> text);
};

#endif  // START_SCENE_H
