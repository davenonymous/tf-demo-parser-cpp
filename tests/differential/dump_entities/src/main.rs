// Copyright 2026 davenonymous
// SPDX-License-Identifier: MIT
//
// Prints every entity's properties at the first packet at or after a demo tick, as JSON.
use std::collections::{BTreeMap, HashMap};
use tf_demo_parser::demo::data::DemoTick;
use tf_demo_parser::demo::message::packetentities::{PacketEntity, UpdateType};
use tf_demo_parser::demo::message::{Message, MessageType};
use tf_demo_parser::demo::packet::datatable::{ParseSendTable, ServerClass};
use tf_demo_parser::demo::parser::MessageHandler;
use tf_demo_parser::demo::sendprop::{SendPropIdentifier, SendPropValue};
use tf_demo_parser::{Demo, DemoParser, ParserState};

struct Dumper {
    target: u32,
    done: bool,
    names: HashMap<SendPropIdentifier, String>,
    classes: Vec<String>,
    entities: BTreeMap<u32, PacketEntity>,
    out: Option<String>,
}

fn val(v: &SendPropValue) -> serde_json::Value {
    match v {
        SendPropValue::Integer(i) => serde_json::json!(i),
        SendPropValue::Float(f) => serde_json::json!(f),
        SendPropValue::Vector(v) => serde_json::json!([v.x, v.y, v.z]),
        SendPropValue::VectorXY(v) => serde_json::json!([v.x, v.y]),
        SendPropValue::String(s) => serde_json::json!(s),
        SendPropValue::Array(a) => serde_json::Value::Array(a.iter().map(val).collect()),
    }
}

impl MessageHandler for Dumper {
    type Output = Option<String>;
    fn does_handle(t: MessageType) -> bool {
        t == MessageType::PacketEntities
    }
    fn handle_data_tables(&mut self, tables: &[ParseSendTable], classes: &[ServerClass], _: &ParserState) {
        for t in tables {
            for p in &t.props {
                self.names.insert(p.identifier, format!("{}.{}", t.name, p.name));
            }
        }
        self.classes = classes.iter().map(|c| c.name.to_string()).collect();
    }
    fn handle_message(&mut self, message: &Message, tick: DemoTick, state: &ParserState) {
        if self.done {
            return;
        }
        if let Message::PacketEntities(m) = message {
            for r in &m.removed_entities {
                self.entities.remove(&u32::from(*r));
            }
            for e in &m.entities {
                let idx = u32::from(e.entity_index);
                match e.update_type {
                    UpdateType::Enter => {
                        let mut n = e.clone();
                        n.props = e.props(state).collect();
                        self.entities.insert(idx, n);
                    }
                    UpdateType::Delta => {
                        if let Some(x) = self.entities.get_mut(&idx) {
                            x.apply_update(&e.props);
                        }
                    }
                    UpdateType::Delete => {
                        self.entities.remove(&idx);
                    }
                    UpdateType::Leave => {}
                }
            }
            if u32::from(tick) >= self.target {
                self.done = true;
                let mut list = Vec::new();
                for (idx, e) in &self.entities {
                    let mut props = serde_json::Map::new();
                    for p in &e.props {
                        let name = self.names.get(&p.identifier).cloned().unwrap_or_default();
                        props.insert(format!("{}:{}", p.index, name), val(&p.value));
                    }
                    let class = &self.classes[usize::from(e.server_class)];
                    list.push(serde_json::json!({"index": idx, "class": class, "props": props}));
                }
                self.out = Some(serde_json::json!({"tick": u32::from(tick), "entities": list}).to_string());
            }
        }
    }
    fn into_output(self, _: &ParserState) -> Self::Output {
        self.out
    }
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let file = std::fs::read(&args[1]).unwrap();
    let demo = Demo::new(&file);
    let dumper = Dumper {
        target: args[2].parse().unwrap(),
        done: false,
        names: HashMap::new(),
        classes: vec![],
        entities: BTreeMap::new(),
        out: None,
    };
    let (_, out) = DemoParser::new_all_with_analyser(demo.get_stream(), dumper).parse().unwrap();
    println!("{}", out.unwrap_or_default());
}
