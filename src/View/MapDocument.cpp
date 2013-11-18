/*
 Copyright (C) 2010-2013 Kristian Duske
 
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

#include "MapDocument.h"

#include "Logger.h"
#include "PreferenceManager.h"
#include "Assets/EntityDefinition.h"
#include "Assets/Texture.h"
#include "Assets/ModelDefinition.h"
#include "IO/DiskFileSystem.h"
#include "IO/SystemPaths.h"
#include "Model/BrushFace.h"
#include "Model/EntityBrushesIterator.h"
#include "Model/EntityFacesIterator.h"
#include "Model/Game.h"
#include "Model/GameFactory.h"
#include "Model/Map.h"
#include "Model/MapFacesIterator.h"
#include "Model/MapObjectsIterator.h"
#include "Model/ModelUtils.h"
#include "Model/SelectionResult.h"
#include "View/MapFrame.h"
#include "View/ViewUtils.h"

#include <cassert>

#include <wx/filedlg.h>
#include <wx/msgdlg.h>

namespace TrenchBroom {
    namespace View {
        class SetEntityDefinition {
        private:
            Assets::EntityDefinitionManager& m_definitionManager;
        public:
            SetEntityDefinition(Assets::EntityDefinitionManager& definitionManager) :
            m_definitionManager(definitionManager) {}
            
            void operator()(Model::Entity* entity) const {
                Assets::EntityDefinition* definition = m_definitionManager.definition(entity);
                entity->setDefinition(definition);
            }
        };
        
        struct UnsetEntityDefinition {
            void operator()(Model::Entity* entity) const {
                entity->setDefinition(NULL);
            }
        };
        
        class SetEntityModel {
        private:
            Assets::EntityModelManager& m_modelManager;
            Logger& m_logger;
        public:
            SetEntityModel(Assets::EntityModelManager& modelManager, Logger& logger) :
            m_modelManager(modelManager),
            m_logger(logger) {}
            
            void operator()(Model::Entity* entity) const {
                const Assets::ModelSpecification spec = entity->modelSpecification();
                if (spec.path.isEmpty()) {
                    entity->setModel(NULL);
                } else {
                    Assets::EntityModel* model = safeGetModel(m_modelManager, spec, m_logger);
                    entity->setModel(model);
                }
            }
        };
        
        struct UnsetEntityModel {
            void operator()(Model::Entity* entity) const {
                entity->setModel(NULL);
            }
        };
        
        class SetTexture {
        private:
            Assets::TextureManager& m_textureManager;
        public:
            SetTexture(Assets::TextureManager& textureManager) :
            m_textureManager(textureManager) {}
            
            void operator()(Model::BrushFace* face) const {
                const String& textureName = face->textureName();
                Assets::Texture* texture = m_textureManager.texture(textureName);
                face->setTexture(texture);
            }
        };
        
        struct UnsetTexture {
            void operator()(Model::BrushFace* face) const {
                face->setTexture(NULL);
            }
        };
        
        class AddToPicker {
        private:
            Model::Picker& m_picker;
        public:
            AddToPicker(Model::Picker& picker) :
            m_picker(picker) {}
            
            void operator()(Model::Object* object) const {
                m_picker.addObject(object);
            }
        };
        
        class RemoveFromPicker {
        private:
            Model::Picker& m_picker;
        public:
            RemoveFromPicker(Model::Picker& picker) :
            m_picker(picker) {}
            
            void operator()(Model::Object* object) const {
                m_picker.removeObject(object);
            }
        };
        
        class AddToMap {
        private:
            Model::Map& m_map;
        public:
            AddToMap(Model::Map& map) :
            m_map(map) {}
            
            void operator()(Model::Entity* entity) const {
                m_map.addEntity(entity);
            }
        };
        
        class RemoveFromMap {
        private:
            Model::Map& m_map;
        public:
            RemoveFromMap(Model::Map& map) :
            m_map(map) {}
            
            void operator()(Model::Entity* entity) const {
                m_map.removeEntity(entity);
            }
        };
        
        class AddToEntity {
        private:
            Model::Entity& m_entity;
        public:
            AddToEntity(Model::Entity& entity) :
            m_entity(entity) {}
            
            void operator()(Model::Brush* brush) const {
                m_entity.addBrush(brush);
            }
        };
        
        class RemoveFromEntity {
        private:
            Model::Entity& m_entity;
        public:
            RemoveFromEntity(Model::Entity& entity) :
            m_entity(entity) {}
            
            void operator()(Model::Brush* brush) const {
                m_entity.removeBrush(brush);
            }
        };
        
        const BBox3 MapDocument::DefaultWorldBounds(-16384.0, 16384.0);
        
        MapDocumentPtr MapDocument::newMapDocument() {
            return MapDocumentPtr(new MapDocument());
        }
        
        MapDocument::~MapDocument() {
            unbindObservers();
            delete m_map;
            m_map = NULL;
        }
        
        const BBox3& MapDocument::worldBounds() const {
            return m_worldBounds;
        }
        
        const IO::Path& MapDocument::path() const {
            return m_path;
        }
        
        String MapDocument::filename() const {
            if (m_path.isEmpty())
                return "";
            return  m_path.lastComponent().asString();
        }
        
        Model::GamePtr MapDocument::game() const {
            return m_game;
        }
        
        Model::Map* MapDocument::map() const {
            return m_map;
        }
        
        const Model::ModelFilter& MapDocument::filter() const {
            return m_filter;
        }
        
        Model::ModelFilter& MapDocument::filter() {
            return m_filter;
        }
        
        Assets::EntityDefinitionManager& MapDocument::entityDefinitionManager() {
            return m_entityDefinitionManager;
        }
        
        Assets::EntityModelManager& MapDocument::entityModelManager() {
            return m_entityModelManager;
        }
        
        Assets::TextureManager& MapDocument::textureManager() {
            return m_textureManager;
        }
        
        View::Grid& MapDocument::grid() {
            return m_grid;
        }
        
        bool MapDocument::isGamePathPreference(const IO::Path& path) const {
            return m_game != NULL && m_game->isGamePathPreference(path);
        }

        bool MapDocument::modified() const {
            return m_modificationCount > 0;
        }
        
        void MapDocument::incModificationCount() {
            ++m_modificationCount;
        }
        
        void MapDocument::decModificationCount() {
            assert(m_modificationCount > 0);
            --m_modificationCount;
        }
        
        void MapDocument::clearModificationCount() {
            m_modificationCount = 0;
        }
        
        void MapDocument::newDocument(const BBox3& worldBounds, Model::GamePtr game) {
            assert(game != NULL);
            info("Creating new document");
            
            m_selection.clear();
            m_worldBounds = worldBounds;
            m_game = game;
            delete m_map;
            m_map = game->newMap(Model::MapFormat::Quake);
            
            m_entityDefinitionManager.clear();
            m_entityModelManager.reset(m_game);
            m_textureManager.reset(m_game);
            m_picker = Model::Picker(m_worldBounds);
            
            setDocumentPath(IO::Path("unnamed.map"));
            clearModificationCount();
            loadAndUpdateEntityDefinitions();
            loadBuiltinTextures();
        }
        
        void MapDocument::openDocument(const BBox3& worldBounds, Model::GamePtr game, const IO::Path& path) {
            assert(game != NULL);
            info("Opening document document " + path.asString());
            
            m_selection.clear();
            m_worldBounds = worldBounds;
            m_game = game;
            delete m_map;
            m_map = m_game->loadMap(worldBounds, path);
            
            m_entityDefinitionManager.clear();
            m_entityModelManager.reset(m_game);
            m_textureManager.reset(m_game);
            m_picker = Model::Picker(m_worldBounds);
            
            setDocumentPath(path);
            clearModificationCount();
            loadAndUpdateEntityDefinitions();
            loadAndUpdateTextures();
            
            Model::each(Model::MapObjectsIterator::begin(*m_map),
                        Model::MapObjectsIterator::end(*m_map),
                        AddToPicker(m_picker),
                        Model::MatchAll());
        }
        
        void MapDocument::saveDocument() {
            assert(!m_path.isEmpty());
            doSaveDocument(m_path);
        }
        
        void MapDocument::saveDocumentAs(const IO::Path& path) {
            doSaveDocument(path);
        }
        
        Model::EntityList MapDocument::parseEntities(const String& str) const {
            return m_game->parseEntities(m_worldBounds, str);
        }
        
        Model::BrushList MapDocument::parseBrushes(const String& str) const {
            return m_game->parseBrushes(m_worldBounds, str);
        }
        
        Model::BrushFaceList MapDocument::parseFaces(const String& str) const {
            return m_game->parseFaces(m_worldBounds, str);
        }
        
        void MapDocument::writeObjectsToStream(const Model::ObjectList& objects, std::ostream& stream) const {
            m_game->writeObjectsToStream(m_map->format(), objects, stream);
        }
        
        void MapDocument::writeFacesToStream(const Model::BrushFaceList& faces, std::ostream& stream) const {
            m_game->writeFacesToStream(m_map->format(), faces, stream);
        }
        
        Model::Entity* MapDocument::worldspawn() {
            Model::Entity* worldspawn = m_map->worldspawn();
            if (worldspawn == NULL) {
                worldspawn = m_map->createEntity();
                worldspawn->addOrUpdateProperty(Model::PropertyKeys::Classname, Model::PropertyValues::WorldspawnClassname);
                addEntity(worldspawn);
                objectWasAddedNotifier(worldspawn);
            }
            return worldspawn;
        }
        
        StringList MapDocument::mods() const {
            return m_game->extractEnabledMods(m_map);
        }

        IO::Path::List MapDocument::definitionFiles() const {
            return m_game->allEntityDefinitionFiles();
        }

        void MapDocument::addObject(Model::Object* object, Model::Object* parent) {
            assert(object != NULL);
            
            if (object->type() == Model::Object::OTEntity)
                addEntity(static_cast<Model::Entity*>(object));
            else if (object->type() == Model::Object::OTBrush) {
                if (parent == NULL) {
                    addBrush(static_cast<Model::Brush*>(object), worldspawn());
                } else {
                    assert(parent->type() == Model::Brush::OTEntity);
                    addBrush(static_cast<Model::Brush*>(object), static_cast<Model::Entity*>(parent));
                }
            }
        }
        
        void MapDocument::removeObject(Model::Object* object) {
            assert(object != NULL);
            assert(object->type() == Model::Object::OTEntity ||
                   object->type() == Model::Object::OTBrush);
            
            if (object->type() == Model::Object::OTEntity)
                removeEntity(static_cast<Model::Entity*>(object));
            else if (object->type() == Model::Object::OTBrush) {
                Model::Brush* brush = static_cast<Model::Brush*>(object);
                removeBrush(brush, brush->parent());
            }
        }
        
        bool MapDocument::hasSelectedObjects() const {
            return m_selection.hasSelectedObjects();
        }
        
        bool MapDocument::hasSelectedEntities() const {
            return m_selection.hasSelectedEntities();
        }
        
        bool MapDocument::hasSelectedBrushes() const {
            return m_selection.hasSelectedBrushes();
        }
        
        bool MapDocument::hasSelectedFaces() const {
            return m_selection.hasSelectedFaces();
        }
        
        bool MapDocument::hasSelection() const {
            return m_selection.hasSelection();
        }
        
        const Model::ObjectList& MapDocument::selectedObjects() const {
            return m_selection.selectedObjects();
        }
        
        const Model::EntityList& MapDocument::selectedEntities() const {
            return m_selection.selectedEntities();
        }
        
        const Model::BrushList& MapDocument::selectedBrushes() const {
            return m_selection.selectedBrushes();
        }
        
        const Model::BrushFaceList& MapDocument::selectedFaces() const {
            return m_selection.selectedFaces();
        }
        
        Model::EntityList MapDocument::allSelectedEntities() const {
            return m_selection.allSelectedEntities();
        }
        
        Model::BrushList MapDocument::allSelectedBrushes() const {
            return m_selection.allSelectedBrushes();
        }
        
        const Model::BrushFaceList& MapDocument::allSelectedFaces() const {
            return m_selection.allSelectedFaces();
        }
        
        Model::EntityList MapDocument::unselectedEntities() const {
            return m_selection.unselectedEntities(*m_map);
        }
        
        Model::BrushList MapDocument::unselectedBrushes() const {
            return m_selection.unselectedBrushes(*m_map);
        }
        
        Model::SelectionResult MapDocument::selectObjects(const Model::ObjectList& objects) {
            return m_selection.selectObjects(objects);
        }
        
        Model::SelectionResult MapDocument::deselectObjects(const Model::ObjectList& objects) {
            return m_selection.deselectObjects(objects);
        }
        
        Model::SelectionResult MapDocument::selectAllObjects() {
            return m_selection.selectAllObjects(*m_map);
        }
        
        Model::SelectionResult MapDocument::selectAllFaces() {
            return m_selection.selectAllFaces(*m_map);
        }
        
        Model::SelectionResult MapDocument::selectFaces(const Model::BrushFaceList& faces) {
            return m_selection.selectFaces(faces);
        }
        
        Model::SelectionResult MapDocument::deselectFaces(const Model::BrushFaceList& faces) {
            return m_selection.deselectFaces(faces);
        }
        
        Model::SelectionResult MapDocument::deselectAll() {
            return m_selection.deselectAll();
        }
        
        Assets::Texture* MapDocument::currentTexture() const {
            if (m_selection.lastSelectedFace() == NULL)
                return NULL;
            return m_selection.lastSelectedFace()->texture();
        }
        
        String MapDocument::currentTextureName() const {
            if (currentTexture() != NULL)
                return currentTexture()->name();
            return Model::BrushFace::NoTextureName;
        }
        
        bool MapDocument::textureLock() const {
            return m_textureLock;
        }
        
        void MapDocument::setTextureLock(const bool textureLock) {
            m_textureLock = textureLock;
        }
        
        void MapDocument::commitPendingRenderStateChanges() {
            m_textureManager.commitChanges();
        }
        
        Model::PickResult MapDocument::pick(const Ray3& ray) {
            return m_picker.pick(ray);
        }
        
        void MapDocument::saveBackup(const IO::Path& path) {
            m_game->writeMap(*m_map, path);
        }
        
        void MapDocument::bindObservers() {
            objectWasAddedNotifier.addObserver(this, &MapDocument::objectWasAdded);
            objectWillBeRemovedNotifier.addObserver(this, &MapDocument::objectWillBeRemoved);
            objectWillChangeNotifier.addObserver(this, &MapDocument::objectWillChange);
            objectDidChangeNotifier.addObserver(this, &MapDocument::objectDidChange);
            modsDidChangeNotifier.addObserver(this, &MapDocument::modsDidChange);
            
            PreferenceManager& prefs = PreferenceManager::instance();
            prefs.preferenceDidChangeNotifier.addObserver(this, &MapDocument::preferenceDidChange);
            
        }
        
        void MapDocument::unbindObservers() {
            objectWasAddedNotifier.removeObserver(this, &MapDocument::objectWasAdded);
            objectWillBeRemovedNotifier.removeObserver(this, &MapDocument::objectWillBeRemoved);
            objectWillChangeNotifier.removeObserver(this, &MapDocument::objectWillChange);
            objectDidChangeNotifier.removeObserver(this, &MapDocument::objectDidChange);
            modsDidChangeNotifier.removeObserver(this, &MapDocument::modsDidChange);
            
            PreferenceManager& prefs = PreferenceManager::instance();
            prefs.preferenceDidChangeNotifier.removeObserver(this, &MapDocument::preferenceDidChange);
        }
        
        void MapDocument::objectWasAdded(Model::Object* object) {
            AddToPicker addToPicker(m_picker);
            addToPicker(object);
            
            if (object->type() == Model::Object::OTEntity) {
                Model::Entity* entity = static_cast<Model::Entity*>(object);
                updateEntityDefinition(entity);
                updateEntityModel(entity);
                
                Model::each(entity->brushes().begin(),
                            entity->brushes().end(),
                            addToPicker,
                            Model::MatchAll());
                
                SetTexture setTexture(m_textureManager);
                Model::each(Model::BrushFacesIterator::begin(entity->brushes()),
                            Model::BrushFacesIterator::end(entity->brushes()),
                            setTexture, Model::MatchAll());
            } else if (object->type() == Model::Object::OTBrush) {
                Model::Brush* brush = static_cast<Model::Brush*>(object);
                SetTexture setTexture(m_textureManager);
                Model::each(brush->faces().begin(),
                            brush->faces().end(),
                            setTexture,
                            Model::MatchAll());
            }
        }
        
        void MapDocument::objectWillBeRemoved(Model::Object* object) {
            RemoveFromPicker removeFromPicker(m_picker);
            removeFromPicker(object);
            
            if (object->type() == Model::Object::OTEntity) {
                Model::Entity* entity = static_cast<Model::Entity*>(object);
                entity->setDefinition(NULL);
                entity->setModel(NULL);
                
                Model::each(entity->brushes().begin(),
                            entity->brushes().end(),
                            removeFromPicker,
                            Model::MatchAll());
                
                UnsetTexture unsetTexture;
                Model::each(Model::BrushFacesIterator::begin(entity->brushes()),
                            Model::BrushFacesIterator::end(entity->brushes()),
                            unsetTexture, Model::MatchAll());
            } else if (object->type() == Model::Object::OTBrush) {
                Model::Brush* brush = static_cast<Model::Brush*>(object);
                UnsetTexture unsetTexture;
                Model::each(brush->faces().begin(),
                            brush->faces().end(),
                            unsetTexture,
                            Model::MatchAll());
            }
        }
        
        void MapDocument::objectWillChange(Model::Object* object) {
            m_picker.removeObject(object);
        }
        
        void MapDocument::objectDidChange(Model::Object* object) {
            m_picker.addObject(object);
            if (object->type() == Model::Object::OTEntity) {
                Model::Entity* entity = static_cast<Model::Entity*>(object);
                updateEntityDefinition(entity);
                updateEntityModel(entity);
            }
        }
        
        void MapDocument::modsDidChange() {
            clearEntityModels();
            updateEntityModels(m_map->entities());
            loadBuiltinTextures();
            updateTextures();
        }

        void MapDocument::preferenceDidChange(const IO::Path& path) {
            if (isGamePathPreference(path)) {
                const Model::GameFactory& gameFactory = Model::GameFactory::instance();
                const IO::Path newGamePath = gameFactory.gamePath(m_game->gameName());
                m_game->setGamePath(newGamePath);
                
                clearEntityModels();
                updateEntityModels(m_map->entities());
                loadBuiltinTextures();
                updateTextures();
            }
        }

        MapDocument::MapDocument() :
        CachingLogger(),
        m_worldBounds(DefaultWorldBounds),
        m_path(""),
        m_map(NULL),
        m_picker(m_worldBounds),
        m_selection(m_filter),
        m_grid(5),
        m_textureLock(true),
        m_modificationCount(0) {
            bindObservers();
        }
        
        void MapDocument::addEntity(Model::Entity* entity) {
            AddToMap addToMap(*m_map);
            addToMap(entity);
        }
        
        void MapDocument::addBrush(Model::Brush* brush, Model::Entity* entity) {
            AddToEntity addToEntity(*entity);
            addToEntity(brush);
        }
        
        void MapDocument::removeEntity(Model::Entity* entity) {
            assert(!entity->worldspawn());
            RemoveFromMap removeFromMap(*m_map);
            removeFromMap(entity);
        }
        
        void MapDocument::removeBrush(Model::Brush* brush, Model::Entity* entity) {
            RemoveFromEntity removeFromEntity(*entity);
            removeFromEntity(brush);
        }
        
        void MapDocument::loadAndUpdateEntityDefinitions() {
            loadEntityDefinitions();
            clearEntityModels();
            updateEntityDefinitions(m_map->entities());
            updateEntityModels(m_map->entities());
        }
        
        void MapDocument::loadEntityDefinitions() {
            const IO::Path path = m_game->extractEntityDefinitionFile(m_map);
            m_entityDefinitionManager.loadDefinitions(m_game, path);
            info("Loaded entity definition file " + path.asString());
        }
        
        void MapDocument::clearEntityModels() {
            m_entityModelManager.clear();
        }
        
        void MapDocument::updateEntityDefinitions(const Model::EntityList& entities) {
            Model::each(entities.begin(),
                        entities.end(),
                        SetEntityDefinition(m_entityDefinitionManager),
                        Model::MatchAll());
        }
        
        void MapDocument::updateEntityDefinition(Model::Entity* entity) {
            SetEntityDefinition setDefinition(m_entityDefinitionManager);
            setDefinition(entity);
        }
        
        void MapDocument::updateEntityModels(const Model::EntityList& entities) {
            Model::each(entities.begin(),
                        entities.end(),
                        SetEntityModel(m_entityModelManager, *this),
                        Model::MatchAll());
        }
        
        void MapDocument::updateEntityModel(Model::Entity* entity) {
            SetEntityModel setModel(m_entityModelManager, *this);
            setModel(entity);
        }
        
        void MapDocument::loadAndUpdateTextures() {
            loadTextures();
            updateTextures();
        }
        
        void MapDocument::loadTextures() {
            loadBuiltinTextures();
            loadExternalTextures();
        }
        
        void MapDocument::loadBuiltinTextures() {
            try {
                const IO::Path::List paths = m_game->findBuiltinTextureCollections();
                m_textureManager.setBuiltinTextureCollections(paths);
                info("Loaded builtin texture collections " + StringUtils::join(IO::Path::asStrings(paths), ", "));
            } catch (Exception e) {
                error(String(e.what()));
            }
        }
        
        void MapDocument::loadExternalTextures() {
            try {
                IO::Path::List rootPaths;
                rootPaths.push_back(IO::SystemPaths::appDirectory());
                if (m_path.isAbsolute())
                    rootPaths.push_back(m_path.deleteLastComponent());
                
                const IO::Path::List texturePaths = m_game->extractTexturePaths(m_map);
                IO::Path::List found, notFound;
                IO::Disk::resolvePaths(rootPaths, texturePaths, found, notFound);
                
                if (!found.empty()) {
                    m_textureManager.addExternalTextureCollections(found);
                    info("Loaded external texture collections " + StringUtils::join(IO::Path::asStrings(found), ", "));
                }
                
                if (!notFound.empty())
                    warn("Could not find external texture collections " + StringUtils::join(IO::Path::asStrings(notFound), ", "));
            } catch (Exception e) {
                error("Error external loading texture collection: %s", e.what());
            }
        }
        
        void MapDocument::updateTextures() {
            Model::each(Model::MapFacesIterator::begin(*m_map),
                        Model::MapFacesIterator::end(*m_map),
                        SetTexture(m_textureManager),
                        Model::MatchAll());
        }
        
        void MapDocument::doSaveDocument(const IO::Path& path) {
            m_game->writeMap(*m_map, path);
            clearModificationCount();
            setDocumentPath(path);
        }
        
        void MapDocument::setDocumentPath(const IO::Path& path) {
            m_path = path;
        }
    }
}
