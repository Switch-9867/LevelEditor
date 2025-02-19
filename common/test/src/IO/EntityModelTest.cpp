/*
 Copyright (C) 2010-2017 Kristian Duske

 This file is part of TrenchBroom.

 TrenchBroom is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 TrenchBroom is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with TrenchBroom. If not, see <http://www.gnu.org/licenses/>.
 */

#include "TestLogger.h"
#include "TestUtils.h"

#include "Assets/EntityModel.h"
#include "Exceptions.h"
#include "IO/EntityModelLoader.h"
#include "IO/Path.h"
#include "Model/Game.h"

#include <optional>

#include "IO/DiskIO.h"
#include "IO/GameConfigParser.h"
#include "Model/GameConfig.h"
#include "Model/GameImpl.h"

#include <vecmath/bbox.h>
#include <vecmath/intersection.h>
#include <vecmath/ray.h>

#include "Catch2.h"

namespace TrenchBroom
{
namespace IO
{
TEST_CASE("BSP model intersection test", "[EntityModelTest]")
{
  auto logger = TestLogger();
  auto [game, gameConfig] = Model::loadGame("Quake");

  const auto path = IO::Path("cube.bsp");

  std::unique_ptr<Assets::EntityModel> model = game->initializeModel(path, logger);
  game->loadFrame(path, 0, *model, logger);

  Assets::EntityModelFrame* frame = model->frames().at(0);

  const auto box = vm::bbox3f(vm::vec3f::fill(-32), vm::vec3f::fill(32));
  CHECK(box == frame->bounds());

  // test some hitting rays
  for (int x = -45; x <= 45; x += 15)
  {
    for (int y = -45; y <= 45; y += 15)
    {
      for (int z = -45; z <= 45; z += 15)
      {
        // shoot a ray from (x, y, z) to (0, 0, 0), it will hit the box
        const auto startPoint = vm::vec3f(x, y, z);
        if (box.contains(startPoint))
        {
          continue;
        }
        const auto endPoint = vm::vec3f::zero();
        const auto ray = vm::ray3f(startPoint, vm::normalize(endPoint - startPoint));

        const float treeDist = frame->intersect(ray);
        const float expected = vm::intersect_ray_bbox(ray, box);

        CHECK(expected == Approx(treeDist));
      }
    }
  }

  // test a missing ray
  const auto missRay = vm::ray3f(vm::vec3f(0, -33, -33), vm::vec3f::pos_y());
  CHECK(vm::is_nan(frame->intersect(missRay)));
  CHECK(vm::is_nan(vm::intersect_ray_bbox(missRay, box)));
}
} // namespace IO
} // namespace TrenchBroom
