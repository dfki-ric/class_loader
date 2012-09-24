/*
 * Copyright (c) 2012, Willow Garage, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Willow Garage, Inc. nor the names of its
 *       contributors may be used to endorse or promote products derived from
 *       this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef CLASS_LOADER_CORE_H_DEFINED
#define CLASS_LOADER_CORE_H_DEFINED

#include <Poco/SharedLibrary.h>
#include <boost/thread/mutex.hpp>
#include <vector>
#include <map>
#include <typeinfo>
#include <string>
#include "class_loader/meta_object.h"
#include "class_loader/class_loader_exceptions.h"

/* Note: This header file is the internal implementation of the plugin system which is exposed via the ClassLoader class
*/

namespace class_loader
{

class ClassLoader; //Forward declaration

namespace class_loader_private
{

//Typedefs
/*****************************************************************************/
typedef std::string LibraryPath;
typedef std::string ClassName;
typedef std::string BaseClassName;
typedef std::map<ClassName, class_loader_private::AbstractMetaObjectBase*> FactoryMap;
typedef std::map<BaseClassName, FactoryMap> BaseToFactoryMapMap;
typedef std::pair<LibraryPath, Poco::SharedLibrary*> LibraryPair;
typedef std::vector<LibraryPair> LibraryVector;

//Global storage
/*****************************************************************************/

/**
 * @brief Gets a handle to a global data structure that holds a map of base class names (Base class describes plugin interface) to a FactoryMap which holds the factories for the various different concrete classes that can be instantiated. Note that the Base class is NOT THE LITERAL CLASSNAME, but rather the result of typeid(Base).name() which sometimes is the literal class name (as on Windows) but is often in mangled form (as on Linux).
 * @return A reference to the global base to factory map
 */
BaseToFactoryMapMap& getGlobalPluginBaseToFactoryMapMap();

/**
 * @brief Gets a handle to a list of open libraries in the form of LibraryPairs which encode the library path+name and the handle to the underlying Poco::SharedLibrary
 * @return A reference to the global vector that tracks loaded libraries
 */
LibraryVector& getLoadedLibraryVector();

/**
 * @brief When a library is being loaded, in order for factories to know which library they are being associated with, they use this function to query which library is being loaded.
 * @return The currently set loading library name as a string
 */
std::string getCurrentlyLoadingLibraryName();

/**
 * @brief When a library is being loaded, in order for factories to know which library they are being associated with, this function is called to set the name of the library currently being loaded.
 * @param library_name - The name of library that is being loaded currently
 */
void setCurrentlyLoadingLibraryName(const std::string& library_name);


/**
 * @brief Gets the ClassLoader currently in scope which used when a library is being loaded.
 * @return A pointer to the currently active ClassLoader.
 */
ClassLoader* getCurrentlyActiveClassLoader();

/**
 * @brief Sets the ClassLoader currently in scope which used when a library is being loaded. 
 * @param loader - pointer to the currently active ClassLoader.
 */
void setCurrentlyActiveClassLoader(ClassLoader* loader);


/**
 * @brief This function extracts a reference to the FactoryMap for appropriate base class out of the global plugin base to factory map. This function should be used by functions in this namespace that need to access the various factories so as to make sure the right key is generated to index into the global map.
 * @return A reference to the FactoryMap contained within the global Base-to-FactoryMap map.
 */
template <typename Base>
FactoryMap& getFactoryMapForBaseClass()
{
  BaseToFactoryMapMap& factoryMapMap = getGlobalPluginBaseToFactoryMapMap();
  std::string base_class_name = typeid(Base).name();
  if(factoryMapMap.find(base_class_name) == factoryMapMap.end())
    factoryMapMap[base_class_name] = FactoryMap();

  return(factoryMapMap[base_class_name]);
}

/**
 * @brief To provide thread safety, all exposed plugin functions can only be run serially by multiple threads. This is implemented by using critical sections enforced by a single mutex which is locked and released with the following two functions
 * @return A reference to the global mutex
 */
boost::mutex& getCriticalSectionMutex();

//Plugin Functions
/*****************************************************************************/

/**
 * @brief This function is called by the REGISTER_CLASS macro in plugin_register_macro.h. Classes that use that macro will cause this function to be invoked when the library is loaded. The function will create a MetaObject (i.e. factory) for the corresponding Derived class and insert it into the appropriate FactoryMap in the global Base-to-FactoryMap map. Note that the passed class_name is the literal class name and not the mangled version.
 * @param Derived - parameteric type indicating concrete type of plugin
 * @param Base - parameteric type indicating base type of plugin
 * @param class_name - the literal name of the class being registered (NOT MANGLED)
 */
template <typename Derived, typename Base> 
void registerPlugin(const std::string& class_name)
{
  //Note: Critical section not necessary as registerPlugin is called within scope of loadLibrary which is protected
  logDebug("class_loader::class_loader_core: Registering plugin class %s.\n", class_name.c_str());

  class_loader_private::AbstractMetaObject<Base>* new_factory = new class_loader_private::MetaObject<Derived, Base>(class_name.c_str());

  logDebug("class_loader::class_loader_core: Registering with class loader = %p and library name %s.\n", getCurrentlyActiveClassLoader(), getCurrentlyLoadingLibraryName().c_str());
  new_factory->addOwningClassLoader(getCurrentlyActiveClassLoader());
  new_factory->setAssociatedLibraryPath(getCurrentlyLoadingLibraryName());

  FactoryMap& factoryMap = getFactoryMapForBaseClass<Base>();
  factoryMap[class_name] = new_factory;

  logDebug("class_loader::class_loader_core: Registration of %s complete.\n", class_name.c_str());
}

/**
 * @brief This function creates an instance of a plugin class given the derived name of the class and returns a pointer of the Base class type.
 * @param derived_class_name - The name of the derived class (unmangled)
 * @param loader - The ClassLoader whose scope we are within
 * @return A pointer to newly created plugin, note caller is responsible for object destruction
 */
template <typename Base> 
Base* createInstance(const std::string& derived_class_name, ClassLoader* loader)
{
  boost::mutex::scoped_lock lock(getCriticalSectionMutex());

  Base* obj = NULL;
  FactoryMap& factoryMap = getFactoryMapForBaseClass<Base>();
  if(factoryMap.find(derived_class_name) != factoryMap.end())
  {
    AbstractMetaObject<Base>* factory = dynamic_cast<class_loader_private::AbstractMetaObject<Base>*>(factoryMap[derived_class_name]);
    if(factory->isOwnedBy(loader))
      obj = factory->create();
  }

  if(obj == NULL) //Was never created
    throw(class_loader::CreateClassException("Could not create instance of type " + derived_class_name));

  return(obj);
}

/**
 * @brief This function returns all the available class_loader in the plugin system that are derived from Base and within scope of the passed ClassLoader.
 * @param loader - The pointer to the ClassLoader whose scope we are within,
 * @return A vector of strings where each string is a plugin we can create
 */
template <typename Base> 
std::vector<std::string> getAvailableClasses(ClassLoader* loader)
{
  boost::mutex::scoped_lock lock(getCriticalSectionMutex());

  FactoryMap& factory_map = getFactoryMapForBaseClass<Base>();
  std::vector<std::string> classes;

  for(FactoryMap::const_iterator itr = factory_map.begin(); itr != factory_map.end(); ++itr)
  {
    AbstractMetaObjectBase* factory = itr->second;
    if(factory->isOwnedBy(loader))
      classes.push_back(itr->first);
  }

  return(classes);
}

/**
 * @brief This function returns the names of all libraries in use by a given class loader.
 * @param loader - The ClassLoader whose scope we are within
 * @return A vector of strings where each string is the path+name of each library that are within a ClassLoader's visible scope
 */
std::vector<std::string> getAllLibrariesUsedByClassLoader(const ClassLoader* loader);

/**
 * @brief Indicates if passed library loaded within scope of a ClassLoader. The library maybe loaded in memory, but to the class loader it may not be.
 * @param library_path - The name of the library we wish to check is open
 * @param loader - The pointer to the ClassLoader whose scope we are within
 * @return true if the library is loaded within loader's scope, else false
 */
bool isLibraryLoaded(const std::string& library_path, ClassLoader* loader);

/**
 * @brief Indicates if passed library has been loaded by ANY ClassLoader
 * @param library_path - The name of the library we wish to check is open
 * @return true if the library is loaded in memory, otherwise false
 */
bool isLibraryLoadedByAnybody(const std::string& library_path);

/**
 * @brief Loads a library into memory if it has not already been done so. Attempting to load an already loaded library has no effect.
 * @param library_path - The name of the library to open
 * @param loader - The pointer to the ClassLoader whose scope we are within
 */
void loadLibrary(const std::string& library_path, ClassLoader* loader);

/**
 * @brief Unloads a library if it loaded in memory and cleans up its corresponding class factories. If it is not loaded, the function has no effect
 * @param library_path - The name of the library to open
 * @param loader - The pointer to the ClassLoader whose scope we are within
 */
void unloadLibrary(const std::string& library_path, ClassLoader* loader);


} //End namespace class_loader_private
} //End namespace class_loader

#endif