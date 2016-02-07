#ifndef CPP_WRAPPER_HPP
#define CPP_WRAPPER_HPP

#include <cassert>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <typeinfo>
#include <typeindex>
#include <vector>

#include "array.hpp"
#include "type_conversion.hpp"

namespace cpp_wrapper
{

/// Some helper functions
namespace detail
{

// Need to treat void specially
template<typename R, typename... Args>
struct ReturnTypeAdapter
{
	inline mapped_julia_type<remove_const_ref<R>> operator()(const void* functor, mapped_julia_type<mapped_reference_type<Args>>... args)
	{
		auto std_func = reinterpret_cast<const std::function<R(Args...)>*>(functor);
		assert(std_func != nullptr);
		return convert_to_julia((*std_func)(convert_to_cpp<mapped_reference_type<Args>>(args)...));
	}
};

template<typename... Args>
struct ReturnTypeAdapter<void, Args...>
{
	inline void operator()(const void* functor, mapped_julia_type<mapped_reference_type<Args>>... args)
	{
		auto std_func = reinterpret_cast<const std::function<void(Args...)>*>(functor);
		assert(std_func != nullptr);
		(*std_func)(convert_to_cpp<mapped_reference_type<Args>>(args)...);
	}
};

/// Call a C++ std::function, passed as a void pointer since it comes from Julia
template<typename R, typename... Args>
mapped_julia_type<remove_const_ref<R>> call_functor(const void* functor, mapped_julia_type<remove_const_ref<Args>>... args)
{
	try
	{
		return ReturnTypeAdapter<R, Args...>()(functor, args...);
	}
	catch(const std::runtime_error& err)
	{
		jl_error(err.what());
		return mapped_julia_type<remove_const_ref<R>>();
	}
}

/// Make a vector with the types in the variadic template parameter pack
template<typename... Args>
std::vector<jl_datatype_t*> typeid_vector()
{
  return {static_type_mapping<remove_const_ref<Args>>::julia_type()...};
}

template<typename... Args>
struct NeedConvertHelper
{
	bool operator()()
	{
		for(const bool b : {std::is_same<mapped_julia_type<remove_const_ref<Args>>,remove_const_ref<Args>>::value...})
		{
			if(!b)
				return true;
		}
		return false;
	}
};

template<>
struct NeedConvertHelper<>
{
	bool operator()()
	{
		return false;
	}
};

/// Finalizer function for type T
template<typename T>
jl_value_t* finalizer(jl_value_t *F, jl_value_t **args, uint32_t nargs)
{
	jl_value_t* to_delete = args[0];

	T* stored_obj = convert_to_cpp<T*>(to_delete);
	if(stored_obj != nullptr)
	{
		delete stored_obj;
	}

	jl_set_nth_field(to_delete, 0, jl_box_voidpointer(nullptr));
}

/// Create a new julia object wrapping the C++ type
template<typename T, typename... ArgsT>
typename std::enable_if<!IsBits<T>::value, jl_value_t*>::type create(ArgsT... args)
{
	jl_datatype_t* dt = static_type_mapping<T>::julia_type();
	assert(!jl_isbits(dt));

	T* cpp_obj = new T(args...);

	jl_value_t* result = jl_new_struct(dt, jl_box_voidpointer(static_cast<void*>(cpp_obj)));
	static jl_function_t* finalizer_func = jl_new_closure(finalizer<T>, (jl_value_t*)jl_emptysvec, NULL);
	jl_gc_add_finalizer(result, finalizer_func);

	return result;
}

template<typename T, typename... ArgsT>
typename std::enable_if<IsBits<T>::value, T>::type create(ArgsT... args)
{
	std::cout << "ceating bits of type " << typeid(T).name() << std::endl;
	jl_datatype_t* dt = static_type_mapping<T>::julia_type();
	assert(jl_isbits(dt));
	return T(args...);
}

} // end namespace detail

// The CppWrapper Julia module
extern jl_module_t* g_cpp_wrapper_module;
extern jl_datatype_t* g_cppfunctioninfo_type;

/// Abstract base class for storing any function
class FunctionWrapperBase
{
public:
	/// Function pointer as void*, since that's what Julia expects
	virtual void* pointer() = 0;

	/// The thunk (i.e. std::function) to pass as first argument to the function pointed to by function_pointer
	virtual void* thunk() = 0;

	/// Types of the arguments
	virtual std::vector<jl_datatype_t*> argument_types() const = 0;

	/// Return type
	virtual jl_datatype_t* return_type() const = 0;

	virtual ~FunctionWrapperBase() {}

	inline void set_name(const std::string& name)
	{
		m_name = name;
	}

	inline const std::string& name() const
	{
		return m_name;
	}

private:
	std::string m_name;
};

/// Implementation of function storage, case of std::function
template<typename R, typename... Args>
class FunctionWrapper : public FunctionWrapperBase
{
public:
	typedef std::function<R(Args...)> functor_t;

	FunctionWrapper(const functor_t& function) : m_function(function)
	{
	}

	virtual void* pointer()
	{
		return reinterpret_cast<void*>(&detail::call_functor<R, Args...>);
	}

	virtual void* thunk()
	{
		return reinterpret_cast<void*>(&m_function);
	}

	virtual std::vector<jl_datatype_t*> argument_types() const
	{
		return detail::typeid_vector<Args...>();
	}

	virtual jl_datatype_t* return_type() const
	{
		return static_type_mapping<R>::julia_type();
	}

private:
	functor_t m_function;
};

/// Implementation of function storage, case of a function pointer
template<typename R, typename... Args>
class FunctionPtrWrapper : public FunctionWrapperBase
{
public:
	typedef std::function<R(Args...)> functor_t;

	FunctionPtrWrapper(R(*f)(Args...)) : m_function(f)
	{
	}

	virtual void* pointer()
	{
		return reinterpret_cast<void*>(m_function);
	}

	virtual void* thunk()
	{
		return nullptr;
	}

	virtual std::vector<jl_datatype_t*> argument_types() const
	{
		return detail::typeid_vector<Args...>();
	}

	virtual jl_datatype_t* return_type() const
	{
		return static_type_mapping<R>::julia_type();
	}

private:
	R(*m_function)(Args...);
};

/// Encapsulate a list of types, for the field list of a Julia composite type
template<typename... TypesT>
struct TypeList
{
	template<typename... StringT>
	TypeList(StringT... names)
	{
		static_assert(sizeof...(TypesT) == sizeof...(StringT), "Number of types must be equal to number of field names");
		field_names = {names...};
	}

	std::vector<std::string> field_names;
};

/// Wrap the base type
template<typename SuperT>
struct Super
{
	typedef SuperT type;
};

/// Represent a Julia TypeVar in the template parameter list
template<int I>
struct TypeVar
{
	static constexpr int value = I;

	static jl_tvar_t* tvar()
	{
		static jl_tvar_t* this_tvar = jl_new_typevar(jl_symbol((std::string("T") + std::to_string(I)).c_str()), (jl_value_t*)jl_bottom_type, (jl_value_t*)jl_any_type);
		return this_tvar;
	}
};

template<typename T>
class TypeWrapper;

/// Store all exposed C++ functions associated with a module
class Module
{
public:

	Module(const std::string& name);

	/// Define a new function
	template<typename R, typename... Args>
	void method(const std::string& name,  std::function<R(Args...)> f)
	{
		auto* new_wrapper = new FunctionWrapper<R, Args...>(f);
		new_wrapper->set_name(name);
		m_functions.resize(m_functions.size()+1);
		m_functions.back().reset(new_wrapper);
	}

	/// Define a new function. Overload for pointers
	template<typename R, typename... Args>
	void method(const std::string& name,  R(*f)(Args...))
	{
		bool need_convert = !std::is_same<mapped_julia_type<R>,remove_const_ref<R>>::value || detail::NeedConvertHelper<Args...>()();

		// Conversion is automatic when using the std::function calling method, so if we need conversion we use that
		if(need_convert)
		{
			method(name, std::function<R(Args...)>(f));
			return;
		}

		// No conversion needed -> call can be through a naked function pointer
		auto* new_wrapper = new FunctionPtrWrapper<R, Args...>(f);
		new_wrapper->set_name(name);
		m_functions.resize(m_functions.size()+1);
		m_functions.back().reset(new_wrapper);
	}

	/// Define a new function. Overload for lambda
	template<typename LambdaT>
	void method(const std::string& name,  LambdaT&& lambda)
	{
		add_lambda(name, lambda, &LambdaT::operator());
	}

	/// Loop over the functions
	template<typename F>
	void for_each_function(const F f) const
	{
		for(const auto& item : m_functions)
		{
			f(*item);
		}
	}

	/// Add a composite type
	template<typename T, typename... ArgsT>
	TypeWrapper<T> add_type(const std::string& name, ArgsT... args);

	/// Add an abstract type
	template<typename T, typename... ArgsT>
	TypeWrapper<T> add_abstract(const std::string& name, ArgsT... args);

	template<typename T, typename... ArgsT>
	void add_parametric(const std::string& name, ArgsT... args);

	template<typename T>
	TypeWrapper<T> apply();

	/// Add type T as a struct that can be captured as bits type, using an immutable in Julia
	template<typename T, typename... ArgsT>
	TypeWrapper<T> add_bits(const std::string& name, ArgsT... args);

	const std::string& name() const
	{
		return m_name;
	}

	void bind_types(jl_module_t* mod)
	{
		for(auto& dt_pair : m_jl_datatypes)
		{
			jl_set_const(mod, jl_symbol(dt_pair.first.c_str()), (jl_value_t*)dt_pair.second);
		}
	}

private:

	template<typename T>
	void add_default_constructor(std::true_type);

	template<typename T>
	void add_default_constructor(std::false_type)
	{
	}

	template<typename R, typename LambdaRefT, typename LambdaT, typename... ArgsT>
	void add_lambda(const std::string& name, LambdaRefT&& lambda, R(LambdaT::*f)(ArgsT...) const)
	{
		method(name, std::function<R(ArgsT...)>(lambda));
	}

	template<typename T>
	void add_copy_constructor(std::true_type)
	{
		method("deepcopy_internal", std::function<jl_value_t*(const T&, ObjectIdDict)>( [this](const T& other, ObjectIdDict)
		{
			return detail::create<T>(other);
		}));
	}

	template<typename T>
	void add_copy_constructor(std::false_type)
	{
		method("deepcopy_internal", std::function<jl_value_t*(const T&, ObjectIdDict)>( [this](const T& other, ObjectIdDict)
		{
			throw std::runtime_error("Copy construction not supported for C++ type ");
			return nullptr;
		}));
	}

	std::string m_name;
	std::vector<std::unique_ptr<FunctionWrapperBase>> m_functions;
	std::map<std::string, jl_datatype_t*> m_jl_datatypes;
};

/// Helper class to wrap type methods
template<typename T>
class TypeWrapper
{
public:
	TypeWrapper(Module& mod) : m_module(mod)
	{
	}

	/// Add a constructor with the given argument types
	template<typename... ArgsT>
	TypeWrapper<T>& constructor()
	{
		m_module.method("call", std::function<mapped_julia_type<T>(SingletonType<T>, ArgsT...)>( [](SingletonType<T>, ArgsT... args) { return detail::create<T>(args...); }));
		return *this;
	}

	/// Define a member function
	template<typename R, typename... ArgsT>
	TypeWrapper<T>& method(const std::string& name, R(T::*f)(ArgsT...))
	{
		m_module.method(name, [f](T& obj, ArgsT... args) { return (obj.*f)(args...); } );
		return *this;
	}

	/// Define a member function, const version
	template<typename R, typename... ArgsT>
	TypeWrapper<T>& method(const std::string& name, R(T::*f)(ArgsT...) const)
	{
		m_module.method(name, [f](const T& obj, ArgsT... args) { return (obj.*f)(args...); } );
		return *this;
	}

protected:
	Module& m_module;
};

template<typename T>
void Module::add_default_constructor(std::true_type)
{
	TypeWrapper<T>(*this).template constructor<>();
}

namespace detail
{

template<template<typename...> class ParameterT, typename T, typename FunctorT>
void process_argument(FunctorT&&, T)
{
}

template<template<typename...> class ParameterT, typename FunctorT, typename... ParameterArgsT>
void process_argument(FunctorT&& f, ParameterT<ParameterArgsT...> arg)
{
	f(arg);
}

template<template<typename...> class ParameterT, typename FunctorT>
void process_arguments(FunctorT&&)
{
}

/// Look for a template ParameterT in the argument list
template<template<typename...> class ParameterT, typename FunctorT, typename ArgT, typename... OtherArgsT>
void process_arguments(FunctorT&& f, ArgT arg, OtherArgsT... other_args)
{
	process_argument<ParameterT>(f, arg);
	process_arguments<ParameterT>(f, other_args...);
}

template<typename T>
struct GetJlType
{
	jl_datatype_t* operator()() const
	{
		return static_type_mapping<T>::julia_type();
	}
};

template<int I>
struct GetJlType<TypeVar<I>>
{
	jl_tvar_t* operator()() const
	{
		return TypeVar<I>::tvar();
	}
};

template<template<typename...> class T, typename... ParametersT>
struct GetJlType<T<ParametersT...>>
{
	jl_datatype_t* operator()() const;
};

template<typename T, T Val>
struct GetJlType<std::integral_constant<T, Val>>
{
	jl_value_t* operator()() const
	{
		return box(convert_to_julia(Val));
	}
};

template<typename T>
struct GetParameters;

template<template<typename...> class T, typename... ParametersT>
struct GetParameters<T<ParametersT...>>
{
	jl_svec_t* operator()()
	{
		return jl_svec(sizeof...(ParametersT), GetJlType<ParametersT>()()...);
	}
};

template<template<typename...> class T, typename... ParametersT>
jl_datatype_t* GetJlType<T<ParametersT...>>::operator()() const
{
	return (jl_datatype_t*)jl_apply_type((jl_value_t*)parametric_type_mapping<T>::julia_type(), GetParameters<T<ParametersT...>>()());
}

template<typename... TypesT>
void build_type_vectors(const TypeList<TypesT...>& typelist, jl_svec_t*& fnames, jl_svec_t*& ftypes, int& ninitialized, bool add_ptr)
{
	static constexpr int nb_types = sizeof...(TypesT);
	const int ptr_offset = add_ptr ? 1 : 0;
	if(add_ptr)
	{
		ftypes = jl_svec(nb_types + ptr_offset, jl_voidpointer_type, GetJlType<TypesT>()()...);
	}
	else
	{
		ftypes = jl_svec(nb_types + ptr_offset, GetJlType<TypesT>()()...);
	}
	fnames = jl_alloc_svec_uninit(nb_types + ptr_offset);

	if(add_ptr)
	{
		jl_svecset(fnames, 0, jl_symbol("cpp_object"));
	}
	for(int i = 0; i != nb_types; ++i)
	{
		jl_svecset(fnames, i+ptr_offset, jl_symbol(typelist.field_names[i].c_str()));
	}
	ninitialized = nb_types+ptr_offset;
}

template<typename T>
struct ParametricTypeMapping;

template<template<typename...> class TemplateT, typename... TypesT>
struct ParametricTypeMapping<TemplateT<TypesT...>> : parametric_type_mapping<TemplateT> {};

template<typename... ArgsT>
void build_type_data(bool add_ptr, jl_datatype_t*& super, jl_svec_t*& fnames, jl_svec_t*& ftypes, int& ninitialized, ArgsT... args)
{
	// Fill fnames and ftypes
	process_arguments<TypeList>([&](auto typelist)
	{
		detail::build_type_vectors(typelist, fnames, ftypes, ninitialized, add_ptr);
	}, args...);

	if(fnames == nullptr)
	{
		assert(ftypes == nullptr);
		detail::build_type_vectors(TypeList<>(), fnames, ftypes, ninitialized, add_ptr);
	}

	process_arguments<Super>([&](auto super_t) { super = GetJlType<typename decltype(super_t)::type>()(); }, args...);
	if(super == nullptr)
	{
		super = static_type_mapping<CppAny>::julia_type();
	}
}

} // namespace detail

template<typename T, typename... ArgsT>
TypeWrapper<T> Module::add_type(const std::string& name, ArgsT... args)
{
	static_assert(!IsBits<T>::value, "Bits types (marked with IsBits) can't be added using add_type, use add_bits instead");
	if(m_jl_datatypes.count(name) > 0)
	{
		throw std::runtime_error("Duplicate registration of type " + name);
	}

	jl_datatype_t* super = nullptr;
	jl_svec_t* parameters = jl_emptysvec;
	jl_svec_t* fnames = nullptr;
	jl_svec_t* ftypes = nullptr;
	int abstract = 0;
	int mutabl = 1;
	int ninitialized = 0;

	JL_GC_PUSH4(super, parameters, fnames, ftypes);

	// Fill fnames and ftypes
	detail::build_type_data(true, super, fnames, ftypes, ninitialized, args...);

	// Create the datatype
	jl_datatype_t* dt = jl_new_datatype(jl_symbol(name.c_str()), super, parameters, fnames, ftypes, abstract, mutabl, ninitialized);

	// Register the type
	static_type_mapping<T>::set_julia_type(dt);
	static_type_mapping<T*>::set_julia_type(dt);
	m_jl_datatypes[name] = dt;
	JL_GC_POP();

	// Add constructors
	add_default_constructor<T>(std::is_default_constructible<T>());
	add_copy_constructor<T>(std::is_copy_constructible<T>());

	return TypeWrapper<T>(*this);
}

template<typename T, typename... ArgsT>
TypeWrapper<T> Module::add_abstract(const std::string& name, ArgsT... args)
{
	if(m_jl_datatypes.count(name) > 0)
	{
		throw std::runtime_error("Duplicate registration of type " + name);
	}

	jl_datatype_t* super = nullptr;
	jl_svec_t* parameters = jl_emptysvec;
	jl_svec_t* fnames = jl_emptysvec;
	jl_svec_t* ftypes = jl_emptysvec;
	int abstract = 1;
	int mutabl = 0;
	int ninitialized = 0;

	JL_GC_PUSH4(super, parameters, fnames, ftypes);

	// Get superclass, if defined
	detail::process_arguments<Super>([&](auto super_t) { super = detail::GetJlType<decltype(super_t)>()(); }, args...);
	if(super == nullptr)
	{
		super = static_type_mapping<CppAny>::julia_type();
	}

	// Create the datatype
	jl_datatype_t* dt = jl_new_datatype(jl_symbol(name.c_str()), super, parameters, fnames, ftypes, abstract, mutabl, ninitialized);

	// Register the type
	static_type_mapping<T>::set_julia_type(dt);
	static_type_mapping<T*>::set_julia_type(dt);
	m_jl_datatypes[name] = dt;
	JL_GC_POP();

	return TypeWrapper<T>(*this);
}

template<typename T, typename... ArgsT>
void Module::add_parametric(const std::string& name, ArgsT... args)
{
	if(m_jl_datatypes.count(name) > 0)
	{
		throw std::runtime_error("Duplicate registration of type " + name);
	}

	jl_datatype_t* super = nullptr;
	jl_svec_t* parameters = nullptr;
	jl_svec_t* fnames = nullptr;
	jl_svec_t* ftypes = nullptr;
	int abstract = 0;
	int mutabl = 1;
	int ninitialized = 0;

	JL_GC_PUSH4(super, parameters, fnames, ftypes);

	// Set the parameters
	parameters = detail::GetParameters<T>()();

	// Fill fnames and ftypes
	detail::build_type_data(true, super, fnames, ftypes, ninitialized, args...);

	// Create the datatype associated with the parametric type
	jl_datatype_t* dt = jl_new_datatype(jl_symbol(name.c_str()), super, parameters, fnames, ftypes, abstract, mutabl, ninitialized);
	detail::ParametricTypeMapping<T>::set_julia_type(dt);

	m_jl_datatypes[name] = dt;
	JL_GC_POP();
}

/// Add a bits type
template<typename T, typename... ArgsT>
TypeWrapper<T> Module::add_bits(const std::string& name, ArgsT... args)
{
	static_assert(std::is_standard_layout<T>::value, "Bits types must be standard layout");
	static_assert(IsBits<T>::value, "Bits types must be marked as such by specializing the IsBits template");
	if(m_jl_datatypes.count(name) > 0)
	{
		throw std::runtime_error("Duplicate registration of type " + name);
	}

	jl_datatype_t* super = nullptr;
	jl_svec_t* parameters = jl_emptysvec;
	jl_svec_t* fnames = nullptr;
	jl_svec_t* ftypes = nullptr;
	int abstract = 0;
	int mutabl = 0;
	int ninitialized = 0;

	JL_GC_PUSH4(super, parameters, fnames, ftypes);

	// Fill fnames and ftypes
	detail::build_type_data(false, super, fnames, ftypes, ninitialized, args...);

	// Create the datatype
	jl_datatype_t* dt = jl_new_datatype(jl_symbol(name.c_str()), super, parameters, fnames, ftypes, abstract, mutabl, ninitialized);

	// Register the type
	static_type_mapping<T>::set_julia_type(dt);
	static_type_mapping<T*>::set_julia_type(dt);
	m_jl_datatypes[name] = dt;
	JL_GC_POP();

	// Add constructors
	add_default_constructor<T>(std::is_default_constructible<T>());

	return TypeWrapper<T>(*this);
}

template<typename T>
TypeWrapper<T> Module::apply()
{
	add_default_constructor<T>(std::is_default_constructible<T>());
	add_copy_constructor<T>(std::is_copy_constructible<T>());
	jl_datatype_t* app_dt = (jl_datatype_t*)jl_apply_type((jl_value_t*)detail::ParametricTypeMapping<T>::julia_type(), detail::GetParameters<T>()());
	static_type_mapping<T>::set_julia_type(app_dt);
	static_type_mapping<T*>::set_julia_type(app_dt);
	return TypeWrapper<T>(*this);
}


/// Registry containing different modules
class ModuleRegistry
{
public:
	/// Create a module and register it
	Module& create_module(const std::string& name);

	/// Loop over the modules
	template<typename F>
	void for_each_module(const F f) const
	{
		for(const auto& item : m_modules)
		{
			f(*item.second);
		}
	}

	Module& get_module(const std::string& name)
	{
		const auto iter = m_modules.find(name);
		if(iter == m_modules.end())
		{
			throw std::runtime_error("Module with name " + name + " was not found in registry");
		}

		return *(iter->second);
	}

private:
	std::map<std::string, std::unique_ptr<Module>> m_modules;
};


} // namespace cpp_wrapper

/// Register a new module
#define JULIA_CPP_MODULE_BEGIN(registry) \
extern "C" void register_julia_modules(void* void_reg) { \
	cpp_wrapper::ModuleRegistry& registry = *reinterpret_cast<cpp_wrapper::ModuleRegistry*>(void_reg);

#define JULIA_CPP_MODULE_END }

#endif
