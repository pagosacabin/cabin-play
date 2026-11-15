"""Default seed exercises for the writing peer review application.

Contains 15 curated exercises across 5 required types:
scene, dialogue, character, description, flash fiction.
Each has a 2-4 sentence creative prompt and a suggested length.
"""

SEED_EXERCISES = [
    # --- SCENE ---
    {
        "type": "scene",
        "title": "The Waiting Room at Midnight",
        "prompt": (
            "Two estranged siblings sit together in a hospital waiting room at 2:00 AM, "
            "waiting for news about their father's condition. Neither has spoken in five years, "
            "and the vending machine between them hums with an erratic rattle. Capture the tension "
            "through small physical actions and the oppressive atmosphere without revealing what "
            "caused the initial rift."
        ),
        "suggested_length": "300-500 words",
    },
    {
        "type": "scene",
        "title": "The Last Train Departure",
        "prompt": (
            "A passenger realizes mid-journey that they have boarded the wrong train, heading into "
            "a countryside they do not recognize as twilight falls. The conductor approaches down the aisle "
            "to punch tickets. Write the scene focusing on the passenger's internal calculation of "
            "whether to confess or pretend they meant to go here."
        ),
        "suggested_length": "250-400 words",
    },
    {
        "type": "scene",
        "title": "The Floodgate Inspection",
        "prompt": (
            "An engineer inspects an aging municipal canal floodgate during the first hour of a "
            "torrential autumn storm. Water pressure is rising faster than recorded in the maintenance logs. "
            "Frame the immediate physical danger alongside the mundane bureaucracy of the clipboard they "
            "are trying to keep dry."
        ),
        "suggested_length": "350-500 words",
    },

    # --- DIALOGUE ---
    {
        "type": "dialogue",
        "title": "Subtext over Boiling Water",
        "prompt": (
            "Write a conversation between two longtime roommates making tea in a cramped kitchen "
            "the morning after an unspoken betrayal. They must only talk about the tea, the electric kettle, "
            "and the weather, but the emotional conflict must be unmistakable. Use no dialogue tags other "
            "than 'said' or none at all."
        ),
        "suggested_length": "250-400 words",
    },
    {
        "type": "dialogue",
        "title": "The Appraisal",
        "prompt": (
            "An antique dealer examines an heirloom pocket watch brought in by someone who clearly "
            "needs money urgently. The dealer knows the watch is worth a small fortune, but suspects the "
            "seller has no idea. Render the negotiation through clipped exchanges, pauses, and the cadence "
            "of voice."
        ),
        "suggested_length": "300-450 words",
    },
    {
        "type": "dialogue",
        "title": "Wrong Number Confession",
        "prompt": (
            "A late-night caller dials a wrong number and begins pouring out an urgent, desperate apology "
            "before the listener can interject. Instead of hanging up, the person who answered decides to "
            "respond in character. Write the phone call entirely in dialogue."
        ),
        "suggested_length": "250-400 words",
    },

    # --- CHARACTER ---
    {
        "type": "character",
        "title": "The Archivist's Hands",
        "prompt": (
            "Introduce a municipal archivist who has spent forty years filing land deeds in a subterranean basement. "
            "Reveal their temperament, hidden obsessions, and private grief solely through how they handle "
            "paper, dust, and their lunch hour. Avoid direct psychological exposition."
        ),
        "suggested_length": "300-500 words",
    },
    {
        "type": "character",
        "title": "The Habitual Complainer",
        "prompt": (
            "Portray a retired transit worker who visits the same diner every morning to complain about the "
            "temperature of the coffee. Through their complaints and interactions with the waitress, reveal "
            "the deep tenderness and need for community lurking beneath the cantankerous exterior. Let their "
            "routine show who they used to be."
        ),
        "suggested_length": "300-450 words",
    },
    {
        "type": "character",
        "title": "The Stage Magician's Morning",
        "prompt": (
            "Describe an aging illusionist preparing breakfast in an unheated apartment before an afternoon matinee. "
            "Detail the unconscious sleight-of-hand habits that have infected their daily motor skills—palming "
            "spoons, misdirecting their own reflection. Reveal what they have sacrificed for the craft."
        ),
        "suggested_length": "350-500 words",
    },

    # --- DESCRIPTION ---
    {
        "type": "description",
        "title": "The Abandoned Greenhouse",
        "prompt": (
            "Describe a Victorian glass greenhouse that has been neglected for three decades, now overgrown with "
            "invasive vines and wild ferns. Focus on the interplay of fractured glass, filtered sunlight, humidity, "
            "and the scent of rotting cedar. Evoke a specific mood of melancholic resilience without using the words "
            "'sad', 'old', or 'broken'."
        ),
        "suggested_length": "200-350 words",
    },
    {
        "type": "description",
        "title": "The Night Shift Bakery",
        "prompt": (
            "Capture the sensory landscape of an industrial bakery operating at 3:30 AM before the city wakes. "
            "Describe the heat of the revolving ovens, the cloud of unbleached flour suspended in halogen light, "
            "the rhythm of metal paddles, and the yeasty sweetness thick in the air. Make the workspace feel alive "
            "and breathing."
        ),
        "suggested_length": "250-400 words",
    },
    {
        "type": "description",
        "title": "The Estuary at Low Tide",
        "prompt": (
            "Render the mudflats of a tidal estuary at dawn when the sea has receded to the horizon. "
            "Detail the glistening silt, the exposed ribcage of an abandoned rowboat, the scavenging shorebirds, "
            "and the sudden pop of air bubbles from burrowing mollusks. Ground the description in tactile and auditory textures."
        ),
        "suggested_length": "250-350 words",
    },

    # --- FLASH FICTION ---
    {
        "type": "flash fiction",
        "title": "The Key That Fits Nothing",
        "prompt": (
            "Write a complete story in under 400 words about a heavy brass key discovered taped under a desk drawer "
            "in a newly purchased house. The protagonist tries it in every lock in the house, finding only one locked "
            "box in the attic. Build to a singular, striking closing image."
        ),
        "suggested_length": "200-400 words",
    },
    {
        "type": "flash fiction",
        "title": "The Perimeter Fence",
        "prompt": (
            "A night security guard at a remote solar installation notices a stray dog sitting outside the chain-link "
            "fence every night at 2:15 AM. Over the course of one week, the guard begins leaving scraps and speaking "
            "to the animal. Conclude the piece at the moment an unexpected decision must be made."
        ),
        "suggested_length": "250-400 words",
    },
    {
        "type": "flash fiction",
        "title": "Before the Rain Hit",
        "prompt": (
            "Tell a complete narrative contained within the three minutes before a severe summer thunderstorm breaks "
            "over a neighborhood block party. Tarp corners snap, children scramble for dropped toys, and an unresolved "
            "confession hangs in the static charge of the air. Every sentence should push the clock closer to the first raindrop."
        ),
        "suggested_length": "250-400 words",
    },
]
